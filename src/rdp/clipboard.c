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

/*
 * The clipboard over cliprdr: CF_UNICODETEXT both ways, nothing copied
 * ahead.
 *
 * Notebook to session: the client's Format List with text makes wayrdp's
 * own selection the session's (rdp_clipboard_handler.offer). A paste comes
 * as an fd; wayrdp asks the client for the text (Format Data Request) and
 * writes the answer, as UTF-8, to every fd that waits.
 *
 * Session to notebook: a selection with text in the session becomes a
 * Format List with CF_UNICODETEXT, no data. When the client asks, wayrdp
 * reads the selection and answers in UTF-16.
 *
 * The channel runs on the main loop: its event handle is an aml handler.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aml.h>
#include <winpr/user.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/wtsvc.h>

#include "rdp/internal.h"
#include "rdp/text.h"
#include "log.h"

#define PASTE_TIMEOUT_US (5 * 1000000)

struct rdp_fd_writer {
	LIST_ENTRY(rdp_fd_writer) link;
	struct aml_handler* handler;
	int fd;
	char* data;
	size_t len, pos;
};

/* ---- writing pastes ---- */

static void writer_free(struct rdp_fd_writer* w)
{
	if (w->handler) {
		aml_stop(aml_get_default(), w->handler);
		aml_unref(w->handler);
	}
	close(w->fd);
	free(w->data);
	LIST_REMOVE(w, link);
	free(w);
}

static void on_writable(struct aml_handler* handler)
{
	struct rdp_fd_writer* w = aml_get_userdata(handler);
	ssize_t n = write(w->fd, w->data + w->pos, w->len - w->pos);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		wv_log(WV_LOG_WARNING, "Writing a paste failed: %m");
		writer_free(w);
		return;
	}
	w->pos += n;
	if (w->pos == w->len)
		writer_free(w);
}

/* Writes data to fd and closes it, in the background if need be. */
static void write_paste(struct rdp_server* server, int fd, const char* data,
		size_t len)
{
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	size_t pos = 0;
	while (pos < len) {
		ssize_t n = write(fd, data + pos, len - pos);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN) {
				wv_log(WV_LOG_WARNING, "Writing a paste failed: %m");
				close(fd);
				return;
			}
			break;
		}
		pos += n;
	}
	if (pos == len) {
		close(fd);
		return;
	}

	struct rdp_fd_writer* w = calloc(1, sizeof(*w));
	if (w)
		w->data = malloc(len - pos);
	if (!w || !w->data) {
		free(w);
		close(fd);
		return;
	}
	memcpy(w->data, data + pos, len - pos);
	w->fd = fd;
	w->len = len - pos;
	w->handler = aml_handler_new(fd, on_writable, w, NULL);
	LIST_INSERT_HEAD(&server->writers, w, link);
	if (!w->handler) {
		writer_free(w);
		return;
	}
	aml_set_event_mask(w->handler, AML_EVENT_WRITE);
	aml_start(aml_get_default(), w->handler);
}

void rdp_clipboard_writers_destroy(struct rdp_server* server)
{
	while (!LIST_EMPTY(&server->writers))
		writer_free(LIST_FIRST(&server->writers));
}

/* ---- pastes waiting for the client ---- */

/* Answers every waiting paste with text, or closes them (text NULL). */
static void finish_pastes(struct rdp_peer* self, const char* text, size_t len)
{
	for (int i = 0; i < self->n_paste_fds; ++i) {
		if (text)
			write_paste(self->server, self->paste_fds[i], text, len);
		else
			close(self->paste_fds[i]);
	}
	self->n_paste_fds = 0;
	self->paste_requested = false;
	if (self->paste_timer)
		aml_stop(aml_get_default(), self->paste_timer);
}

static void on_paste_timeout(struct aml_timer* timer)
{
	struct rdp_peer* self = aml_get_userdata(timer);
	wv_log(WV_LOG_WARNING, "The client didn't send its clipboard in %d s",
			PASTE_TIMEOUT_US / 1000000);
	finish_pastes(self, NULL, 0);
}

static const struct rdp_clipboard_handler* handler(const struct rdp_peer* self)
{
	return self->server->config.clipboard;
}

static void* userdata(const struct rdp_peer* self)
{
	return self->server->config.userdata;
}

void rdp_clipboard_paste(struct rdp_peer* self, int fd)
{
	if (!self->clip_ready || !self->client_has_text
			|| self->n_paste_fds == (int)(sizeof(self->paste_fds)
				/ sizeof(self->paste_fds[0]))) {
		close(fd);
		return;
	}

	self->paste_fds[self->n_paste_fds++] = fd;
	if (self->paste_requested)
		return;

	CLIPRDR_FORMAT_DATA_REQUEST request = {
		.common.msgType = CB_FORMAT_DATA_REQUEST,
		.requestedFormatId = CF_UNICODETEXT,
	};
	if (self->cliprdr->ServerFormatDataRequest(self->cliprdr, &request)) {
		finish_pastes(self, NULL, 0);
		return;
	}
	self->paste_requested = true;

	if (!self->paste_timer)
		self->paste_timer = aml_timer_new(PASTE_TIMEOUT_US,
				on_paste_timeout, self, NULL);
	if (self->paste_timer)
		aml_start(aml_get_default(), self->paste_timer);
}

/* ---- from the client ---- */

static struct rdp_peer* peer_of(CliprdrServerContext* context)
{
	return context->custom;
}

static UINT on_client_capabilities(CliprdrServerContext* context,
		const CLIPRDR_CAPABILITIES* caps)
{
	return CHANNEL_RC_OK;
}

static UINT on_temp_directory(CliprdrServerContext* context,
		const CLIPRDR_TEMP_DIRECTORY* dir)
{
	return CHANNEL_RC_OK;
}

static UINT on_client_format_list(CliprdrServerContext* context,
		const CLIPRDR_FORMAT_LIST* list)
{
	struct rdp_peer* self = peer_of(context);

	bool has_text = false;
	for (UINT32 i = 0; i < list->numFormats; ++i)
		if (list->formats[i].formatId == CF_UNICODETEXT)
			has_text = true;

	CLIPRDR_FORMAT_LIST_RESPONSE response = {
		.common.msgType = CB_FORMAT_LIST_RESPONSE,
		.common.msgFlags = CB_RESPONSE_OK,
	};
	UINT rc = context->ServerFormatListResponse(context, &response);

	/* A paste for the old clipboard won't get an answer now. */
	if (self->paste_requested)
		finish_pastes(self, NULL, 0);

	bool first = !self->clip_ready;
	self->clip_ready = true;
	self->client_has_text = has_text;
	wv_log(WV_LOG_DEBUG, "Client clipboard: %s (%u formats)",
			has_text ? "text" : "no text", list->numFormats);

	if (self->server->active == self && handler(self)) {
		if (has_text)
			handler(self)->offer(userdata(self));
		else if (!first)
			handler(self)->withdraw(userdata(self));
	}

	/* The client's list comes first, then the session's. */
	if (first && !has_text)
		rdp_clipboard_announce(self);
	return rc;
}

static UINT on_client_format_list_response(CliprdrServerContext* context,
		const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
	return CHANNEL_RC_OK;
}

static void send_data_response(struct rdp_peer* self, const uint8_t* data,
		size_t len)
{
	CLIPRDR_FORMAT_DATA_RESPONSE response = {
		.common.msgType = CB_FORMAT_DATA_RESPONSE,
		.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL,
		.common.dataLen = data ? len : 0,
		.requestedFormatData = data,
	};
	self->cliprdr->ServerFormatDataResponse(self->cliprdr, &response);
}

struct read_request {
	struct rdp_server* server;
	uint64_t serial;
};

static void on_session_text(void* cbdata, const char* text, size_t len)
{
	struct read_request* req = cbdata;
	struct rdp_server* server = req->server;
	uint64_t serial = req->serial;
	free(req);

	/* The client may have gone meanwhile. */
	struct rdp_peer* self = server->active;
	if (!self || self->serial != serial || !self->cliprdr)
		return;

	if (!text) {
		send_data_response(self, NULL, 0);
		return;
	}

	size_t out_len = 0;
	uint8_t* utf16 = rdp_utf8_to_utf16le(text, len, &out_len);
	if (!utf16 || out_len > RDP_CLIPBOARD_MAX) {
		send_data_response(self, NULL, 0);
		free(utf16);
		return;
	}
	wv_log(WV_LOG_DEBUG, "Sending %zu bytes of clipboard to the client", len);
	send_data_response(self, utf16, out_len);
	free(utf16);
}

static UINT on_client_format_data_request(CliprdrServerContext* context,
		const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	struct rdp_peer* self = peer_of(context);

	if (request->requestedFormatId != CF_UNICODETEXT || !handler(self)
			|| self->server->active != self) {
		send_data_response(self, NULL, 0);
		return CHANNEL_RC_OK;
	}

	struct read_request* req = calloc(1, sizeof(*req));
	if (!req) {
		send_data_response(self, NULL, 0);
		return CHANNEL_RC_OK;
	}
	req->server = self->server;
	req->serial = self->serial;

	if (handler(self)->read(userdata(self), on_session_text, req) < 0) {
		free(req);
		send_data_response(self, NULL, 0);
	}
	return CHANNEL_RC_OK;
}

static UINT on_client_format_data_response(CliprdrServerContext* context,
		const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	struct rdp_peer* self = peer_of(context);
	if (!self->paste_requested)
		return CHANNEL_RC_OK;

	if (!(response->common.msgFlags & CB_RESPONSE_OK)) {
		finish_pastes(self, NULL, 0);
		return CHANNEL_RC_OK;
	}

	size_t len = response->common.dataLen;
	if (len > RDP_CLIPBOARD_MAX) {
		wv_log(WV_LOG_WARNING, "Client clipboard larger than %u MiB, not pasted",
				RDP_CLIPBOARD_MAX >> 20);
		finish_pastes(self, NULL, 0);
		return CHANNEL_RC_OK;
	}

	size_t text_len = 0;
	char* text = rdp_utf16le_to_utf8(response->requestedFormatData, len,
			&text_len);
	wv_log(WV_LOG_DEBUG, "Pasting %zu bytes from the client", text_len);
	finish_pastes(self, text, text_len);
	free(text);
	return CHANNEL_RC_OK;
}

/* ---- the channel ---- */

static void on_channel_event(struct aml_handler* handler)
{
	struct rdp_peer* self = aml_get_userdata(handler);
	if (self->closing || !self->cliprdr)
		return;
	if (self->cliprdr->CheckEventHandle(self->cliprdr) != CHANNEL_RC_OK) {
		wv_log(WV_LOG_WARNING, "Clipboard channel failed; no clipboard");
		rdp_clipboard_close(self);
	}
}

void rdp_clipboard_open(struct rdp_peer* self)
{
	if (self->cliprdr || !handler(self))
		return;

	struct rdp_peer_context* ctx =
		(struct rdp_peer_context*)self->client->context;
	if (!WTSVirtualChannelManagerIsChannelJoined(ctx->vcm,
				CLIPRDR_SVC_CHANNEL_NAME)) {
		wv_log(WV_LOG_INFO, "%s: no clipboard channel", self->client->hostname);
		return;
	}

	CliprdrServerContext* c = cliprdr_server_context_new(ctx->vcm);
	if (!c)
		return;
	c->custom = self;
	c->rdpcontext = self->client->context;
	c->useLongFormatNames = TRUE;
	c->streamFileClipEnabled = FALSE;
	c->fileClipNoFilePaths = TRUE;
	c->canLockClipData = FALSE;
	c->autoInitializationSequence = FALSE;
	c->ClientCapabilities = on_client_capabilities;
	c->TempDirectory = on_temp_directory;
	c->ClientFormatList = on_client_format_list;
	c->ClientFormatListResponse = on_client_format_list_response;
	c->ClientFormatDataRequest = on_client_format_data_request;
	c->ClientFormatDataResponse = on_client_format_data_response;

	if (c->Open(c) != CHANNEL_RC_OK) {
		wv_log(WV_LOG_WARNING, "Can't open the clipboard channel");
		cliprdr_server_context_free(c);
		return;
	}
	self->cliprdr = c;

	int fd = GetEventFileDescriptor(c->GetEventHandle(c));
	self->cliprdr_handler = fd >= 0
		? aml_handler_new(fd, on_channel_event, self, NULL) : NULL;
	if (!self->cliprdr_handler
			|| aml_start(aml_get_default(), self->cliprdr_handler) < 0) {
		wv_log(WV_LOG_WARNING, "Can't watch the clipboard channel");
		rdp_clipboard_close(self);
		return;
	}

	/* What FreeRDP's own channel thread would send first. */
	CLIPRDR_GENERAL_CAPABILITY_SET general = {
		.capabilitySetType = CB_CAPSTYPE_GENERAL,
		.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN,
		.version = CB_CAPS_VERSION_2,
		.generalFlags = CB_USE_LONG_FORMAT_NAMES,
	};
	CLIPRDR_CAPABILITIES caps = {
		.common.msgType = CB_CLIP_CAPS,
		.common.dataLen = 4 + CB_CAPSTYPE_GENERAL_LEN,
		.cCapabilitiesSets = 1,
		.capabilitySets = (CLIPRDR_CAPABILITY_SET*)&general,
	};
	CLIPRDR_MONITOR_READY ready = {
		.common.msgType = CB_MONITOR_READY,
	};
	if (c->ServerCapabilities(c, &caps) || c->MonitorReady(c, &ready)) {
		wv_log(WV_LOG_WARNING, "Clipboard setup failed");
		rdp_clipboard_close(self);
		return;
	}
	wv_log(WV_LOG_DEBUG, "%s: clipboard channel open", self->client->hostname);
}

void rdp_clipboard_close(struct rdp_peer* self)
{
	finish_pastes(self, NULL, 0);
	if (self->paste_timer) {
		aml_stop(aml_get_default(), self->paste_timer);
		aml_unref(self->paste_timer);
		self->paste_timer = NULL;
	}
	if (self->cliprdr_handler) {
		aml_stop(aml_get_default(), self->cliprdr_handler);
		aml_unref(self->cliprdr_handler);
		self->cliprdr_handler = NULL;
	}
	if (self->cliprdr) {
		self->cliprdr->Close(self->cliprdr);
		cliprdr_server_context_free(self->cliprdr);
		self->cliprdr = NULL;
	}
	self->clip_ready = false;
	self->client_has_text = false;
}

void rdp_clipboard_announce(struct rdp_peer* self)
{
	if (!self->cliprdr || !self->clip_ready)
		return;

	CLIPRDR_FORMAT text = { .formatId = CF_UNICODETEXT };
	CLIPRDR_FORMAT_LIST list = {
		.common.msgType = CB_FORMAT_LIST,
		.numFormats = self->server->session_has_text ? 1 : 0,
		.formats = self->server->session_has_text ? &text : NULL,
	};
	if (list.numFormats)
		wv_log(WV_LOG_DEBUG, "Telling the client: session clipboard has text");
	self->cliprdr->ServerFormatList(self->cliprdr, &list);
}
