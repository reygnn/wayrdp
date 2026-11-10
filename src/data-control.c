/*
 * Copyright (c) 2020 Scott Moreau
 * Copyright (c) 2020 - 2026 Andri Yngvason
 * Copyright (c) 2026 reygnn
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aml.h>
#include <wayland-client.h>

#include "data-control.h"
#include "ext-data-control-v1.h"
#include "wlr-data-control-unstable-v1.h"
#include "log.h"
#include "rdp/text.h"
#include "vec.h"

/* How long a program may take to hand over its clipboard. */
#define READ_TIMEOUT_US (5 * 1000000)

/* Text types, the preferred first. All are read and written as UTF-8. */
static const char* const text_mimes[] = {
	"text/plain;charset=utf-8",
	"UTF8_STRING",
	"text/plain",
	"TEXT",
	"STRING",
};
#define N_TEXT_MIMES (sizeof(text_mimes) / sizeof(text_mimes[0]))
/* Read only these: TEXT and STRING may be Latin-1. */
#define N_READ_MIMES 3

struct dc_offer {
	struct data_control* dc;
	void* proxy;
	bool own;
	int best;	/* index into text_mimes, -1: no text */
};

struct dc_read {
	LIST_ENTRY(dc_read) link;
	struct data_control* dc;
	struct aml_handler* handler;
	struct aml_timer* timer;
	int fd;
	struct wv_vec buffer;
	void (*done)(void*, const char*, size_t);
	void* userdata;
};

static bool is_ext(const struct data_control* self)
{
	return self->ext_manager != NULL;
}

/* ---- the two protocols ---- */

static void offer_receive(struct data_control* self, void* proxy,
		const char* mime, int fd)
{
	if (is_ext(self))
		ext_data_control_offer_v1_receive(proxy, mime, fd);
	else
		zwlr_data_control_offer_v1_receive(proxy, mime, fd);
}

static void offer_free(struct dc_offer* offer)
{
	if (!offer)
		return;
	if (is_ext(offer->dc))
		ext_data_control_offer_v1_destroy(offer->proxy);
	else
		zwlr_data_control_offer_v1_destroy(offer->proxy);
	free(offer);
}

static void source_destroy(struct data_control* self, void* source)
{
	if (is_ext(self))
		ext_data_control_source_v1_destroy(source);
	else
		zwlr_data_control_source_v1_destroy(source);
}

static void set_selection(struct data_control* self, void* source)
{
	if (is_ext(self))
		ext_data_control_device_v1_set_selection(self->device, source);
	else
		zwlr_data_control_device_v1_set_selection(self->device, source);
}

/* ---- offers from the session ---- */

static void on_offer_mime(struct dc_offer* offer, const char* mime)
{
	if (strcmp(mime, offer->dc->own_mime) == 0) {
		offer->own = true;
		return;
	}
	for (int i = 0; i < N_READ_MIMES; ++i)
		if (strcmp(mime, text_mimes[i]) == 0
				&& (offer->best < 0 || i < offer->best))
			offer->best = i;
}

static void ext_offer_mime(void* data, struct ext_data_control_offer_v1* proxy,
		const char* mime)
{
	on_offer_mime(data, mime);
}

static void wlr_offer_mime(void* data, struct zwlr_data_control_offer_v1* proxy,
		const char* mime)
{
	on_offer_mime(data, mime);
}

static const struct ext_data_control_offer_v1_listener ext_offer_listener = {
	.offer = ext_offer_mime,
};
static const struct zwlr_data_control_offer_v1_listener wlr_offer_listener = {
	.offer = wlr_offer_mime,
};

static void on_data_offer(struct data_control* self, void* proxy)
{
	struct dc_offer* offer = calloc(1, sizeof(*offer));
	if (!offer) {
		/* Can't track it: let it go. */
		if (is_ext(self))
			ext_data_control_offer_v1_destroy(proxy);
		else
			zwlr_data_control_offer_v1_destroy(proxy);
		return;
	}
	offer->dc = self;
	offer->proxy = proxy;
	offer->best = -1;

	/* The offer before had no selection event: forget it. */
	offer_free(self->pending);
	self->pending = offer;

	if (is_ext(self))
		ext_data_control_offer_v1_add_listener(proxy,
				&ext_offer_listener, offer);
	else
		zwlr_data_control_offer_v1_add_listener(proxy,
				&wlr_offer_listener, offer);
}

/* The offer of a selection or primary selection event; NULL for none. */
static struct dc_offer* take_offer(struct data_control* self, void* proxy)
{
	if (!proxy)
		return NULL;
	struct dc_offer* offer = self->pending;
	if (offer && offer->proxy == proxy) {
		self->pending = NULL;
		return offer;
	}
	/* Unknown to us (shouldn't happen): wrap it to destroy it. */
	offer = calloc(1, sizeof(*offer));
	if (offer) {
		offer->dc = self;
		offer->proxy = proxy;
		offer->best = -1;
	}
	return offer;
}

static void notify(struct data_control* self, bool has_text)
{
	if (self->has_text == has_text && !has_text)
		return;
	self->has_text = has_text;
	if (self->callbacks->on_selection_changed)
		self->callbacks->on_selection_changed(self->userdata, has_text);
}

static void on_selection(struct data_control* self, void* proxy)
{
	struct dc_offer* offer = take_offer(self, proxy);

	offer_free(self->current);
	self->current = NULL;

	if (offer && offer->own) {
		/* wayrdp's own: the client has that text already. */
		offer_free(offer);
		self->has_text = false;
		return;
	}

	if (offer && offer->best >= 0) {
		self->current = offer;
		wv_log(WV_LOG_DEBUG, "Session clipboard: text (%s)",
				text_mimes[offer->best]);
		notify(self, true);
		return;
	}

	offer_free(offer);
	wv_log(WV_LOG_DEBUG, "Session clipboard: %s", proxy ? "no text" : "empty");
	notify(self, false);
}

static void on_primary_selection(struct data_control* self, void* proxy)
{
	offer_free(take_offer(self, proxy));
}

static void ext_data_offer(void* data, struct ext_data_control_device_v1* dev,
		struct ext_data_control_offer_v1* id)
{
	on_data_offer(data, id);
}

static void ext_selection(void* data, struct ext_data_control_device_v1* dev,
		struct ext_data_control_offer_v1* id)
{
	on_selection(data, id);
}

static void ext_primary(void* data, struct ext_data_control_device_v1* dev,
		struct ext_data_control_offer_v1* id)
{
	on_primary_selection(data, id);
}

static void ext_finished(void* data, struct ext_data_control_device_v1* dev)
{
	struct data_control* self = data;
	ext_data_control_device_v1_destroy(dev);
	self->device = NULL;
}

static const struct ext_data_control_device_v1_listener ext_device_listener = {
	.data_offer = ext_data_offer,
	.selection = ext_selection,
	.finished = ext_finished,
	.primary_selection = ext_primary,
};

static void wlr_data_offer(void* data, struct zwlr_data_control_device_v1* dev,
		struct zwlr_data_control_offer_v1* id)
{
	on_data_offer(data, id);
}

static void wlr_selection(void* data, struct zwlr_data_control_device_v1* dev,
		struct zwlr_data_control_offer_v1* id)
{
	on_selection(data, id);
}

static void wlr_primary(void* data, struct zwlr_data_control_device_v1* dev,
		struct zwlr_data_control_offer_v1* id)
{
	on_primary_selection(data, id);
}

static void wlr_finished(void* data, struct zwlr_data_control_device_v1* dev)
{
	struct data_control* self = data;
	zwlr_data_control_device_v1_destroy(dev);
	self->device = NULL;
}

static const struct zwlr_data_control_device_v1_listener wlr_device_listener = {
	.data_offer = wlr_data_offer,
	.selection = wlr_selection,
	.finished = wlr_finished,
	.primary_selection = wlr_primary,
};

/* ---- reading the session's clipboard ---- */

static void read_finish(struct dc_read* ctx, bool ok)
{
	aml_stop(aml_get_default(), ctx->handler);
	aml_unref(ctx->handler);
	aml_stop(aml_get_default(), ctx->timer);
	aml_unref(ctx->timer);
	close(ctx->fd);
	LIST_REMOVE(ctx, link);

	if (ok)
		ctx->done(ctx->userdata, ctx->buffer.data ? ctx->buffer.data : "",
				ctx->buffer.len);
	else
		ctx->done(ctx->userdata, NULL, 0);

	wv_vec_destroy(&ctx->buffer);
	free(ctx);
}

static void on_read(struct aml_handler* handler)
{
	struct dc_read* ctx = aml_get_userdata(handler);
	char buf[65536];
	ssize_t n = read(ctx->fd, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		wv_log(WV_LOG_WARNING, "Reading the clipboard failed: %m");
		read_finish(ctx, false);
		return;
	}
	if (n == 0) {
		read_finish(ctx, true);
		return;
	}
	if (ctx->buffer.len + n > RDP_CLIPBOARD_MAX) {
		wv_log(WV_LOG_WARNING, "Clipboard larger than %u MiB, not sent",
				RDP_CLIPBOARD_MAX >> 20);
		read_finish(ctx, false);
		return;
	}
	wv_vec_append(&ctx->buffer, buf, n);
}

static void on_read_timeout(struct aml_timer* timer)
{
	struct dc_read* ctx = aml_get_userdata(timer);
	wv_log(WV_LOG_WARNING, "The program with the clipboard didn't answer in %d s",
			READ_TIMEOUT_US / 1000000);
	read_finish(ctx, false);
}

int data_control_read(struct data_control* self,
		void (*done)(void*, const char*, size_t), void* userdata)
{
	if (!self->current || self->current->best < 0)
		return -1;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		wv_log(WV_LOG_ERROR, "pipe: %m");
		return -1;
	}

	struct dc_read* ctx = calloc(1, sizeof(*ctx));
	if (!ctx || wv_vec_init(&ctx->buffer, 4096) < 0) {
		free(ctx);
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	ctx->dc = self;
	ctx->fd = fds[0];
	ctx->done = done;
	ctx->userdata = userdata;
	ctx->handler = aml_handler_new(ctx->fd, on_read, ctx, NULL);
	ctx->timer = aml_timer_new(READ_TIMEOUT_US, on_read_timeout, ctx, NULL);
	if (!ctx->handler || !ctx->timer) {
		aml_unref(ctx->handler);
		aml_unref(ctx->timer);
		wv_vec_destroy(&ctx->buffer);
		free(ctx);
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	offer_receive(self, self->current->proxy,
			text_mimes[self->current->best], fds[1]);
	close(fds[1]);

	aml_start(aml_get_default(), ctx->handler);
	aml_start(aml_get_default(), ctx->timer);
	LIST_INSERT_HEAD(&self->reads, ctx, link);
	return 0;
}

/* ---- wayrdp's own selection ---- */

static void on_source_send(struct data_control* self, void* source,
		const char* mime, int fd)
{
	if (strcmp(mime, self->own_mime) == 0) {
		static const char marker[] = "wayrdp";
		/* Best effort; it is only a marker. */
		ssize_t n = write(fd, marker, sizeof(marker) - 1);
		(void)n;
		close(fd);
		return;
	}
	if (self->callbacks->on_paste)
		self->callbacks->on_paste(self->userdata, fd);
	else
		close(fd);
}

static void on_source_cancelled(struct data_control* self, void* source)
{
	if (self->own_source == source)
		self->own_source = NULL;
	source_destroy(self, source);
}

static void ext_source_send(void* data, struct ext_data_control_source_v1* src,
		const char* mime, int32_t fd)
{
	on_source_send(data, src, mime, fd);
}

static void ext_source_cancelled(void* data,
		struct ext_data_control_source_v1* src)
{
	on_source_cancelled(data, src);
}

static const struct ext_data_control_source_v1_listener ext_source_listener = {
	.send = ext_source_send,
	.cancelled = ext_source_cancelled,
};

static void wlr_source_send(void* data, struct zwlr_data_control_source_v1* src,
		const char* mime, int32_t fd)
{
	on_source_send(data, src, mime, fd);
}

static void wlr_source_cancelled(void* data,
		struct zwlr_data_control_source_v1* src)
{
	on_source_cancelled(data, src);
}

static const struct zwlr_data_control_source_v1_listener wlr_source_listener = {
	.send = wlr_source_send,
	.cancelled = wlr_source_cancelled,
};

void data_control_offer(struct data_control* self)
{
	if (!self->device)
		return;

	void* source;
	if (is_ext(self)) {
		source = ext_data_control_manager_v1_create_data_source(
				self->ext_manager);
		ext_data_control_source_v1_add_listener(source,
				&ext_source_listener, self);
		for (size_t i = 0; i < N_TEXT_MIMES; ++i)
			ext_data_control_source_v1_offer(source, text_mimes[i]);
		ext_data_control_source_v1_offer(source, self->own_mime);
	} else {
		source = zwlr_data_control_manager_v1_create_data_source(
				self->wlr_manager);
		zwlr_data_control_source_v1_add_listener(source,
				&wlr_source_listener, self);
		for (size_t i = 0; i < N_TEXT_MIMES; ++i)
			zwlr_data_control_source_v1_offer(source, text_mimes[i]);
		zwlr_data_control_source_v1_offer(source, self->own_mime);
	}
	if (!source)
		return;

	set_selection(self, source);
	if (self->own_source)
		source_destroy(self, self->own_source);
	self->own_source = source;
}

void data_control_withdraw(struct data_control* self)
{
	if (!self->own_source)
		return;
	if (self->device)
		set_selection(self, NULL);
	source_destroy(self, self->own_source);
	self->own_source = NULL;
}

/* ---- life ---- */

void data_control_init(struct data_control* self, struct wl_seat* seat,
		const struct data_control_callbacks* callbacks, void* userdata)
{
	self->callbacks = callbacks;
	self->userdata = userdata;
	self->pending = NULL;
	self->current = NULL;
	self->has_text = false;
	self->own_source = NULL;
	LIST_INIT(&self->reads);
	snprintf(self->own_mime, sizeof(self->own_mime),
			"x-wayrdp-client-%08x", (unsigned int)rand());

	if (self->ext_manager) {
		self->device = ext_data_control_manager_v1_get_data_device(
				self->ext_manager, seat);
		ext_data_control_device_v1_add_listener(self->device,
				&ext_device_listener, self);
	} else if (self->wlr_manager) {
		self->device = zwlr_data_control_manager_v1_get_data_device(
				self->wlr_manager, seat);
		zwlr_data_control_device_v1_add_listener(self->device,
				&wlr_device_listener, self);
	} else {
		wv_log(WV_LOG_PANIC, "No data control manager available");
	}
}

void data_control_destroy(struct data_control* self)
{
	while (!LIST_EMPTY(&self->reads))
		read_finish(LIST_FIRST(&self->reads), false);

	if (self->own_source)
		source_destroy(self, self->own_source);
	self->own_source = NULL;

	offer_free(self->pending);
	offer_free(self->current);
	self->pending = NULL;
	self->current = NULL;

	if (self->device) {
		if (is_ext(self))
			ext_data_control_device_v1_destroy(self->device);
		else
			zwlr_data_control_device_v1_destroy(self->device);
	}
	self->device = NULL;
}
