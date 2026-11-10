/*
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
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aml.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>
#include <freerdp/freerdp.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include <freerdp/codec/color.h>
#include <winpr/ssl.h>
#include <winpr/wtsapi.h>

#include "rdp/internal.h"
#include "buffer.h"
#include "log.h"
#include "time-util.h"

/* Without an acknowledgement for this long, stop waiting for one. */
#define ACK_TIMEOUT_US 250000
#define STATS_INTERVAL_US (10 * 1000000)

static void notify_wants_frames(struct rdp_server* self)
{
	if (self->config.on_wants_frames_changed)
		self->config.on_wants_frames_changed(self->config.userdata);
}

static uint32_t pixel_format_from_drm(uint32_t drm)
{
	switch (drm) {
	case DRM_FORMAT_XRGB8888: return PIXEL_FORMAT_BGRX32;
	case DRM_FORMAT_ARGB8888: return PIXEL_FORMAT_BGRA32;
	case DRM_FORMAT_XBGR8888: return PIXEL_FORMAT_RGBX32;
	case DRM_FORMAT_ABGR8888: return PIXEL_FORMAT_RGBA32;
	}
	return 0;
}

static void drop_current(struct rdp_server* self)
{
	wv_buffer_release(self->current);
	self->current = NULL;
	pixman_region_clear(&self->pending);
}

static void on_stats_tick(struct aml_ticker* ticker)
{
	struct rdp_server* self = aml_get_userdata(ticker);
	if (!self->active || !self->active->activated)
		return;

	double secs = STATS_INTERVAL_US / 1e6;
	double avg_ms = self->encode_count
		? self->encode_us_sum / 1000.0 / self->encode_count : 0;
	wv_log(WV_LOG_DEBUG, "%.1f fps, %.0f kB/s, encoding %.1f ms per frame",
			self->frames_window / secs,
			self->bytes_window / 1024.0 / secs, avg_ms);
	self->frames_window = 0;
	self->bytes_window = 0;
	self->encode_us_sum = 0;
	self->encode_count = 0;
}

struct rdp_server* rdp_server_new(const struct rdp_server_config* config)
{
	static bool initialised = false;
	if (!initialised) {
		WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
		winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
		initialised = true;
	}

	struct rdp_server* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->config = *config;
	LIST_INIT(&self->peers);
	pixman_region_init(&self->pending);
	self->next_serial = 1;

	wv_log(WV_LOG_INFO, "FreeRDP %s", rdp_freerdp_version());

	if (access(config->sam_file, R_OK) != 0) {
		wv_log(WV_LOG_ERROR, "No password set (%s missing). Run wayrdp --set-password first",
				config->sam_file);
		goto failure;
	}

	if (rdp_auth_ensure_certificate(config->certificate_file,
				config->private_key_file) < 0)
		goto failure;
	rdp_auth_log_fingerprint(config->certificate_file);

	if (rdp_encoder_init(&self->encoder) < 0)
		goto failure;

	if (aml_require_workers(aml_get_default(), 1) < 0)
		goto failure;

	self->stats_ticker = aml_ticker_new(STATS_INTERVAL_US, on_stats_tick,
			self, NULL);
	aml_start(aml_get_default(), self->stats_ticker);

	if (rdp_listener_open(self) < 0)
		goto failure;

	return self;

failure:
	rdp_server_destroy(self);
	return NULL;
}

void rdp_server_destroy(struct rdp_server* self)
{
	if (!self)
		return;

	rdp_listener_close(self);

	while (!LIST_EMPTY(&self->peers))
		rdp_peer_destroy(LIST_FIRST(&self->peers));
	self->active = NULL;

	if (self->ack_timer) {
		aml_stop(aml_get_default(), self->ack_timer);
		aml_unref(self->ack_timer);
	}
	if (self->stats_ticker) {
		aml_stop(aml_get_default(), self->stats_ticker);
		aml_unref(self->stats_ticker);
	}

	/* A job still in the worker holds the server; let it finish. */
	while (self->encoding) {
		aml_poll(aml_get_default(), 100000);
		aml_dispatch(aml_get_default());
	}

	drop_current(self);
	pixman_region_fini(&self->pending);
	rdp_encoder_fini(&self->encoder);
	free(self);
}

bool rdp_server_wants_frames(const struct rdp_server* self)
{
	return self->active && rdp_peer_wants_frames(self->active);
}

void rdp_server_set_desktop_size(struct rdp_server* self, uint32_t width,
		uint32_t height)
{
	if (self->current)
		return; /* The frames know better. */
	self->width = width;
	self->height = height;
}

void rdp_server_peer_logged_on(struct rdp_server* self, struct rdp_peer* peer)
{
	rdp_auth_success(&self->auth);

	/* One connection at a time: the new one takes over, as when mstsc
	 * reconnects. Only now, so a failed login can't throw anyone out. */
	if (self->active && self->active != peer)
		rdp_input_release(self->active);

	struct rdp_peer* other;
	LIST_FOREACH(other, &self->peers, link) {
		if (other != peer && !other->closing) {
			wv_log(WV_LOG_INFO, "New connection replaces the one from %s",
					other->client->hostname);
			rdp_peer_close(other);
		}
	}
	self->active = peer;
	/* No frames until it is active. */
	notify_wants_frames(self);
}

void rdp_server_peer_activated(struct rdp_server* self, struct rdp_peer* peer)
{
	if (self->active != peer)
		return;

	/* A new session (or a reactivation after a resize) starts from a whole
	 * picture with fresh codec state. */
	self->need_full = true;
	self->encoder_reset = true;
	if (self->current)
		pixman_region_union_rect(&self->pending, &self->pending, 0, 0,
				self->width, self->height);

	notify_wants_frames(self);
	rdp_server_schedule(self);
}

void rdp_server_peer_gone(struct rdp_server* self, struct rdp_peer* peer)
{
	if (!peer->logged_on)
		rdp_auth_failure(&self->auth, gettime_us());

	if (self->active == peer) {
		/* Nothing stays pressed when the client goes. */
		rdp_input_release(peer);
		self->active = NULL;
		/* No one looks; the next client starts from a fresh capture. */
		drop_current(self);
		notify_wants_frames(self);
	}
}

void rdp_server_refresh(struct rdp_server* self, const RECTANGLE_16* rects,
		int n_rects)
{
	if (n_rects == 0) {
		pixman_region_union_rect(&self->pending, &self->pending, 0, 0,
				self->width, self->height);
	} else {
		for (int i = 0; i < n_rects; ++i)
			pixman_region_union_rect(&self->pending, &self->pending,
					rects[i].left, rects[i].top,
					rects[i].right - rects[i].left,
					rects[i].bottom - rects[i].top);
	}
	rdp_server_schedule(self);
}

void rdp_server_output_changed(struct rdp_server* self)
{
	notify_wants_frames(self);
	rdp_server_schedule(self);
}

static void on_ack_timeout(struct aml_timer* timer)
{
	struct rdp_server* self = aml_get_userdata(timer);
	struct rdp_peer* peer = self->active;
	if (!peer)
		return;

	if (!peer->ack_seen) {
		wv_log(WV_LOG_INFO, "The client doesn't acknowledge frames; sending without waiting");
		peer->ack_wait = false;
	} else {
		/* Lost or skipped (mstsc skips them while inactive). */
		peer->last_ack = peer->frame_id;
	}
	rdp_server_schedule(self);
}

static void start_ack_timer(struct rdp_server* self)
{
	if (!self->ack_timer)
		self->ack_timer = aml_timer_new(ACK_TIMEOUT_US, on_ack_timeout,
				self, NULL);
	if (!aml_is_started(aml_get_default(), self->ack_timer))
		aml_start(aml_get_default(), self->ack_timer);
}

static void stop_ack_timer(struct rdp_server* self)
{
	if (self->ack_timer)
		aml_stop(aml_get_default(), self->ack_timer);
}

static void on_encode(struct aml_work* work)
{
	struct rdp_encode_job* job = aml_get_userdata(work);
	rdp_encode(&job->server->encoder, job);
}

static void on_encode_done(struct aml_work* work)
{
	struct rdp_encode_job* job = aml_get_userdata(work);
	struct rdp_server* self = job->server;
	self->encoding = false;

	struct rdp_peer* peer = self->active;
	if (job->ok && peer && peer->serial == job->peer_serial
			&& rdp_peer_wants_frames(peer)) {
		uint64_t before = peer->bytes_sent;
		rdp_peer_send(peer, job);
		self->stats.frames_sent++;
		self->frames_window++;
		self->bytes_window += peer->bytes_sent - before;
		self->encode_us_sum += job->encode_us;
		self->encode_count++;
	} else if (!job->ok) {
		wv_log(WV_LOG_ERROR, "Encoding failed");
	}

	rdp_encode_job_free(job);
	rdp_server_schedule(self);
}

static void free_job_work(void* userdata)
{
	/* The job is freed in on_encode_done; nothing to do here. */
}

/* The damage as rectangles inside the buffer. */
static int collect_rects(struct rdp_server* self, RFX_RECT** out)
{
	pixman_region_intersect_rect(&self->pending, &self->pending, 0, 0,
			self->width, self->height);

	int n = 0;
	const pixman_box16_t* box = pixman_region_rectangles(&self->pending, &n);
	if (n == 0)
		return 0;

	RFX_RECT* rects = calloc(n, sizeof(*rects));
	if (!rects)
		return -1;
	for (int i = 0; i < n; ++i) {
		rects[i].x = box[i].x1;
		rects[i].y = box[i].y1;
		rects[i].width = box[i].x2 - box[i].x1;
		rects[i].height = box[i].y2 - box[i].y1;
	}
	*out = rects;
	return n;
}

void rdp_server_schedule(struct rdp_server* self)
{
	struct rdp_peer* peer = self->active;
	if (!peer || !rdp_peer_wants_frames(peer) || self->encoding
			|| !self->current)
		return;

	if (!pixman_region_not_empty(&self->pending))
		return;

	if (peer->ack_wait && rdp_peer_frames_in_flight(peer)
			>= RDP_MAX_FRAMES_IN_FLIGHT) {
		start_ack_timer(self);
		return;
	}
	stop_ack_timer(self);

	/* The output changed size: the client follows, then reactivates. */
	if (!rdp_peer_size_matches(peer, self->width, self->height)) {
		if (!rdp_peer_resize(peer, self->width, self->height))
			rdp_peer_close(peer);
		return;
	}

	struct rdp_encode_job* job = calloc(1, sizeof(*job));
	if (!job)
		return;

	job->n_rects = collect_rects(self, &job->rects);
	if (job->n_rects <= 0) {
		free(job);
		return;
	}
	pixman_region_clear(&self->pending);

	rdpSettings* settings = peer->client->context->settings;
	job->server = self;
	job->peer_serial = peer->serial;
	job->codec = rdp_peer_codec(peer);
	job->buffer = wv_buffer_ref(self->current);
	job->width = self->width;
	job->height = self->height;
	job->stride = self->current->stride;
	job->pixel_format = self->pixel_format;
	job->rlgr_mode = freerdp_settings_get_uint32(settings,
			FreeRDP_RemoteFxRlgrMode);
	job->max_request_size = freerdp_settings_get_uint32(settings,
			FreeRDP_MultifragMaxRequestSize);
	job->reset = self->encoder_reset;
	self->encoder_reset = false;

	struct aml_work* work = aml_work_new(on_encode, on_encode_done, job,
			free_job_work);
	if (!work || aml_start(aml_get_default(), work) < 0) {
		wv_log(WV_LOG_ERROR, "Can't start the encoder");
		if (work)
			aml_unref(work);
		rdp_encode_job_free(job);
		return;
	}
	aml_unref(work);
	self->encoding = true;
}

void rdp_frame_submit(struct rdp_server* self, struct wv_buffer* buffer,
		const struct pixman_region16* damage)
{
	self->stats.frames++;

	uint32_t format = pixel_format_from_drm(buffer->format);
	if (!format) {
		wv_log(WV_LOG_ERROR, "Unsupported pixel format %.4s",
				(const char*)&buffer->format);
		return;
	}

	bool resized = (uint32_t)buffer->width != self->width
		|| (uint32_t)buffer->height != self->height;
	if (resized) {
		wv_log(WV_LOG_INFO, "Desktop is %dx%d", buffer->width,
				buffer->height);
		self->width = buffer->width;
		self->height = buffer->height;
		self->encoder_reset = true;
	}
	self->pixel_format = format;

	if (self->current != buffer) {
		wv_buffer_release(self->current);
		self->current = wv_buffer_ref(buffer);
	}

	if (resized || self->need_full) {
		pixman_region_union_rect(&self->pending, &self->pending, 0, 0,
				self->width, self->height);
		self->need_full = false;
	} else {
		pixman_region_union(&self->pending, &self->pending,
				(pixman_region16_t*)damage);
	}

	rdp_server_schedule(self);
}

void rdp_server_set_selection_text(struct rdp_server* self, const char* text,
		size_t len)
{
	wv_trace("Selection changed (%zu bytes); clipboard comes in M3", len);
}

void rdp_server_get_stats(const struct rdp_server* self,
		struct rdp_server_stats* stats)
{
	*stats = self->stats;
}

const char* rdp_freerdp_version(void)
{
	return freerdp_get_version_string();
}
