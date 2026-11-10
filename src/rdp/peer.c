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

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <aml.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/synch.h>

#include "rdp/internal.h"
#include "log.h"
#include "time-util.h"

static struct rdp_peer* peer_from_context(rdpContext* context)
{
	return ((struct rdp_peer_context*)context)->peer;
}

static BOOL context_new(freerdp_peer* client, rdpContext* context)
{
	struct rdp_peer_context* ctx = (struct rdp_peer_context*)context;
	ctx->vcm = WTSOpenServerA((LPSTR)client->context);
	return ctx->vcm && ctx->vcm != INVALID_HANDLE_VALUE;
}

static void context_free(freerdp_peer* client, rdpContext* context)
{
	struct rdp_peer_context* ctx = (struct rdp_peer_context*)context;
	if (ctx && ctx->vcm && ctx->vcm != INVALID_HANDLE_VALUE)
		WTSCloseServer(ctx->vcm);
}

static void stop_handlers(struct rdp_peer* self)
{
	for (int i = 0; i < self->n_handlers; ++i) {
		aml_stop(aml_get_default(), self->handlers[i]);
		aml_unref(self->handlers[i]);
	}
	self->n_handlers = 0;
	if (self->vcm_handler) {
		aml_stop(aml_get_default(), self->vcm_handler);
		aml_unref(self->vcm_handler);
		self->vcm_handler = NULL;
	}
}

static void on_close_idle(struct aml_idle* idle)
{
	rdp_peer_destroy(aml_get_userdata(idle));
}

void rdp_peer_close(struct rdp_peer* self)
{
	if (self->closing)
		return;
	self->closing = true;
	self->activated = false;
	stop_handlers(self);

	/* Not from inside the peer's own callbacks. */
	self->close_idle = aml_idle_new(on_close_idle, self, NULL);
	aml_start(aml_get_default(), self->close_idle);
}

void rdp_peer_destroy(struct rdp_peer* self)
{
	stop_handlers(self);
	if (self->close_idle) {
		aml_stop(aml_get_default(), self->close_idle);
		aml_unref(self->close_idle);
		self->close_idle = NULL;
	}

	LIST_REMOVE(self, link);
	rdp_server_peer_gone(self->server, self);

	if (self->activated || self->frames_sent)
		wv_log(WV_LOG_INFO, "%s disconnected after %.0f s, %u frames, %.1f MB",
				self->client->hostname,
				(gettime_us() - self->connected_us) / 1e6,
				self->frames_sent, self->bytes_sent / 1048576.0);
	else
		wv_log(WV_LOG_INFO, "Connection from %s closed",
				self->client->hostname);

	freerdp_peer* client = self->client;
	client->Disconnect(client);
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	free(self);
}

static void on_peer_event(struct aml_handler* handler)
{
	struct rdp_peer* self = aml_get_userdata(handler);
	if (self->closing)
		return;

	if (!self->client->CheckFileDescriptor(self->client)) {
		rdp_peer_close(self);
		return;
	}

	struct rdp_peer_context* ctx =
		(struct rdp_peer_context*)self->client->context;
	if (!WTSVirtualChannelManagerCheckFileDescriptor(ctx->vcm))
		rdp_peer_close(self);
}

/* After NLA. NTLM checked the password against the SAM file, which holds
 * only the user wayrdp runs as; the name the client sent along (in the
 * settings, from TSCredentials) must be that user too. */
static BOOL on_logon(freerdp_peer* client, const SEC_WINNT_AUTH_IDENTITY* identity,
		BOOL automatic)
{
	struct rdp_peer* self = peer_from_context(client->context);

	if (!automatic) {
		wv_log(WV_LOG_WARNING, "%s: logon without NLA refused", client->hostname);
		return FALSE;
	}

	rdpSettings* settings = client->context->settings;
	const char* user = freerdp_settings_get_string(settings, FreeRDP_Username);
	const char* me = rdp_auth_user();
	if (!user || !me || strcasecmp(user, me) != 0) {
		wv_log(WV_LOG_WARNING, "%s: user %s refused", client->hostname,
				user ? user : "(none)");
		return FALSE;
	}

	wv_log(WV_LOG_INFO, "%s: logged on as %s", client->hostname, user);
	self->logged_on = true;
	rdp_server_peer_logged_on(self->server, self);
	return TRUE;
}

static BOOL on_post_connect(freerdp_peer* client)
{
	rdpSettings* settings = client->context->settings;
	wv_log(WV_LOG_INFO, "%s: client wants %ux%u at %u bpp", client->hostname,
			freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
			freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
			freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth));
	return TRUE;
}

enum rdp_codec rdp_peer_codec(const struct rdp_peer* self)
{
	/* For the tests: every client at hand speaks RemoteFX. */
	const char* force = getenv("WAYRDP_FORCE_PLANAR");
	if (force && strcmp(force, "1") == 0)
		return RDP_CODEC_PLANAR;

	rdpSettings* settings = self->client->context->settings;
	uint32_t surface_cmds = freerdp_settings_get_uint32(settings,
			FreeRDP_SurfaceCommandsSupported);
	if (freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec)
			&& (surface_cmds & SURFCMDS_STREAM_SURFACE_BITS)
			&& freerdp_settings_get_uint32(settings,
				FreeRDP_RemoteFxCodecId) != 0)
		return RDP_CODEC_REMOTEFX;
	return RDP_CODEC_PLANAR;
}

static BOOL on_activate(freerdp_peer* client)
{
	struct rdp_peer* self = peer_from_context(client->context);
	struct rdp_server* server = self->server;
	rdpSettings* settings = client->context->settings;

	if (!self->logged_on)
		return FALSE;

	/* The desktop has the output's size, not the window's (until M4). */
	if (server->width && server->height
			&& !rdp_peer_size_matches(self, server->width, server->height))
		return rdp_peer_resize(self, server->width, server->height);

	wv_log(WV_LOG_DEBUG, "%s: RemoteFX %d (id %u, mode %u, RLGR %u), surface commands 0x%x, frame markers %d, multifrag %u",
			client->hostname,
			freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
			freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId),
			freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecMode),
			freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxRlgrMode),
			freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported),
			freerdp_settings_get_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled),
			freerdp_settings_get_uint32(settings, FreeRDP_MultifragMaxRequestSize));

	enum rdp_codec codec = rdp_peer_codec(self);
	self->frame_markers = codec == RDP_CODEC_REMOTEFX
		&& freerdp_settings_get_bool(settings,
				FreeRDP_SurfaceFrameMarkerEnabled);
	self->ack_wait = self->frame_markers;
	self->ack_seen = false;
	self->frame_id = 0;
	self->last_ack = 0;
	self->suppressed = false;
	self->activated = true;
	if (!self->connected_us)
		self->connected_us = gettime_us();

	wv_log(WV_LOG_INFO, "%s: active, %ux%u, %s%s", client->hostname,
			freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
			freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
			codec == RDP_CODEC_REMOTEFX ? "RemoteFX" : "Planar",
			self->frame_markers ? " with frame markers" : "");

	rdp_server_peer_activated(server, self);
	return TRUE;
}

static BOOL on_refresh_rect(rdpContext* context, BYTE count,
		const RECTANGLE_16* areas)
{
	struct rdp_peer* self = peer_from_context(context);
	if (self->server->active == self)
		rdp_server_refresh(self->server, areas, count);
	return TRUE;
}

static BOOL on_suppress_output(rdpContext* context, BYTE allow,
		const RECTANGLE_16* area)
{
	struct rdp_peer* self = peer_from_context(context);
	self->suppressed = !allow;
	wv_log(WV_LOG_DEBUG, "Client %s output", allow ? "resumes" : "suppresses");
	if (self->server->active != self)
		return TRUE;

	rdp_server_output_changed(self->server);
	if (allow)
		rdp_server_refresh(self->server, NULL, 0);
	return TRUE;
}

static BOOL on_frame_ack(rdpContext* context, UINT32 frame_id)
{
	struct rdp_peer* self = peer_from_context(context);
	self->last_ack = frame_id;
	self->ack_seen = true;
	if (self->server->active == self)
		rdp_server_schedule(self->server);
	return TRUE;
}

static bool apply_settings(struct rdp_peer* self)
{
	const struct rdp_server_config* config = &self->server->config;
	rdpSettings* settings = self->client->context->settings;

	rdpCertificate* cert = freerdp_certificate_new_from_file(
			config->certificate_file);
	rdpPrivateKey* key = freerdp_key_new_from_file_enc(
			config->private_key_file, NULL);
	if (!cert || !key) {
		wv_log(WV_LOG_ERROR, "Can't load %s or %s", config->certificate_file,
				config->private_key_file);
		freerdp_certificate_free(cert);
		freerdp_key_free(key);
		return false;
	}
	/* The settings own them from here. */
	if (!freerdp_settings_set_pointer_len(settings,
				FreeRDP_RdpServerCertificate, cert, 1)
			|| !freerdp_settings_set_pointer_len(settings,
				FreeRDP_RdpServerRsaKey, key, 1))
		return false;

	struct { FreeRDP_Settings_Keys_Bool key; bool value; } bools[] = {
		/* NLA only: TLS with CredSSP against the SAM file. */
		{ FreeRDP_NlaSecurity, true },
		{ FreeRDP_TlsSecurity, false },
		{ FreeRDP_RdpSecurity, false },
		{ FreeRDP_ExtSecurity, false },
		{ FreeRDP_RemoteFxCodec, true },
		{ FreeRDP_NSCodec, false },
		{ FreeRDP_SurfaceFrameMarkerEnabled, true },
		{ FreeRDP_FrameMarkerCommandEnabled, true },
		{ FreeRDP_SupportGraphicsPipeline, false },
		{ FreeRDP_SupportDisplayControl, false },
		{ FreeRDP_DesktopResize, true },
		{ FreeRDP_RedirectClipboard, false },
		{ FreeRDP_SuppressOutput, true },
		{ FreeRDP_RefreshRect, true },
		{ FreeRDP_UnicodeInput, true },
		{ FreeRDP_HasHorizontalWheel, true },
		{ FreeRDP_HasExtendedMouseEvent, true },
		{ FreeRDP_DrawAllowSkipAlpha, true },
	};
	for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); ++i)
		if (!freerdp_settings_set_bool(settings, bools[i].key,
					bools[i].value))
			return false;

	if (!freerdp_settings_set_string(settings, FreeRDP_NtlmSamFile,
				config->sam_file)
			|| !freerdp_settings_set_uint32(settings,
				FreeRDP_ColorDepth, 32))
		return false;

	if (self->server->width && self->server->height
			&& (!freerdp_settings_set_uint32(settings,
					FreeRDP_DesktopWidth, self->server->width)
				|| !freerdp_settings_set_uint32(settings,
					FreeRDP_DesktopHeight, self->server->height)))
		return false;

	return true;
}

static int watch_handle(struct rdp_peer* self, HANDLE handle,
		struct aml_handler** out)
{
	int fd = GetEventFileDescriptor(handle);
	if (fd < 0)
		return 0;
	struct aml_handler* handler = aml_handler_new(fd, on_peer_event, self,
			NULL);
	if (!handler || aml_start(aml_get_default(), handler) < 0) {
		aml_unref(handler);
		return -1;
	}
	*out = handler;
	return 1;
}

struct rdp_peer* rdp_peer_new(struct rdp_server* server, freerdp_peer* client)
{
	struct rdp_peer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	self->server = server;
	self->client = client;
	self->serial = server->next_serial++;

	client->ContextSize = sizeof(struct rdp_peer_context);
	client->ContextNew = context_new;
	client->ContextFree = context_free;
	if (!freerdp_peer_context_new(client)) {
		free(self);
		return NULL;
	}
	((struct rdp_peer_context*)client->context)->peer = self;

	if (!apply_settings(self))
		goto failure;

	client->Logon = on_logon;
	client->PostConnect = on_post_connect;
	client->Activate = on_activate;

	rdpUpdate* update = client->context->update;
	update->RefreshRect = on_refresh_rect;
	update->SuppressOutput = on_suppress_output;
	update->SurfaceFrameAcknowledge = on_frame_ack;

	/* Input comes with M2; FreeRDP ignores events without a handler. */

	if (!client->Initialize(client))
		goto failure;

	HANDLE handles[RDP_MAX_PEER_HANDLES];
	DWORD n = client->GetEventHandles(client, handles,
			RDP_MAX_PEER_HANDLES);
	for (DWORD i = 0; i < n; ++i) {
		struct aml_handler* handler = NULL;
		int rc = watch_handle(self, handles[i], &handler);
		if (rc < 0)
			goto failure;
		if (rc > 0)
			self->handlers[self->n_handlers++] = handler;
	}
	if (self->n_handlers == 0)
		goto failure;

	struct rdp_peer_context* ctx = (struct rdp_peer_context*)client->context;
	if (watch_handle(self, WTSVirtualChannelManagerGetEventHandle(ctx->vcm),
				&self->vcm_handler) < 0)
		goto failure;

	LIST_INSERT_HEAD(&server->peers, self, link);
	wv_log(WV_LOG_INFO, "Connection from %s", client->hostname);
	return self;

failure:
	wv_log(WV_LOG_ERROR, "Can't set up the connection from %s",
			client->hostname);
	stop_handlers(self);
	/* The listener frees the peer itself when we fail. */
	freerdp_peer_context_free(client);
	free(self);
	return NULL;
}

bool rdp_peer_wants_frames(const struct rdp_peer* self)
{
	return self->activated && !self->suppressed && !self->closing;
}

uint32_t rdp_peer_frames_in_flight(const struct rdp_peer* self)
{
	if (!self->frame_markers)
		return 0;
	return self->frame_id - self->last_ack;
}

bool rdp_peer_size_matches(const struct rdp_peer* self, uint32_t width,
		uint32_t height)
{
	rdpSettings* settings = self->client->context->settings;
	return freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == width
		&& freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)
			== height;
}

bool rdp_peer_resize(struct rdp_peer* self, uint32_t width, uint32_t height)
{
	rdpContext* context = self->client->context;
	rdpSettings* settings = context->settings;

	if (!freerdp_settings_get_bool(settings, FreeRDP_DesktopResize)) {
		wv_log(WV_LOG_ERROR, "%s can't change the desktop size to %ux%u",
				self->client->hostname, width, height);
		return false;
	}

	wv_log(WV_LOG_INFO, "%s: desktop resized to %ux%u", self->client->hostname,
			width, height);

	/* The client deactivates and comes back through Activate. */
	self->activated = false;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width)
			|| !freerdp_settings_set_uint32(settings,
				FreeRDP_DesktopHeight, height))
		return false;
	return context->update->DesktopResize(context);
}

static void send_remotefx(struct rdp_peer* self, const struct rdp_encode_job* job)
{
	rdpContext* context = self->client->context;
	rdpUpdate* update = context->update;
	rdpSettings* settings = context->settings;

	if (job->n_messages == 0)
		return;

	SURFACE_BITS_COMMAND cmd = { 0 };
	cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
	cmd.bmp.codecID = freerdp_settings_get_uint32(settings,
			FreeRDP_RemoteFxCodecId);
	cmd.destLeft = 0;
	cmd.destTop = 0;
	cmd.destRight = job->width;
	cmd.destBottom = job->height;
	cmd.bmp.bpp = 32;
	cmd.bmp.width = job->width;
	cmd.bmp.height = job->height;
	cmd.skipCompression = TRUE;

	uint32_t frame_id = self->frame_markers ? ++self->frame_id : 0;

	for (size_t i = 0; i < job->n_messages; ++i) {
		wStream* s = job->messages[i];
		cmd.bmp.bitmapDataLength = Stream_GetPosition(s);
		cmd.bmp.bitmapData = Stream_Buffer(s);
		self->bytes_sent += cmd.bmp.bitmapDataLength;

		BOOL ok = self->frame_markers
			? update->SurfaceFrameBits(context, &cmd, i == 0,
					i + 1 == job->n_messages, frame_id)
			: update->SurfaceBits(context, &cmd);
		if (!ok) {
			wv_log(WV_LOG_ERROR, "Sending a frame failed");
			rdp_peer_close(self);
			return;
		}
	}
	self->frames_sent++;
}

static void send_planar(struct rdp_peer* self, const struct rdp_encode_job* job)
{
	rdpContext* context = self->client->context;
	rdpUpdate* update = context->update;

	if (job->n_bitmaps == 0)
		return;

	BITMAP_DATA* data = calloc(job->n_bitmaps, sizeof(*data));
	if (!data)
		return;

	/* Several bitmaps per update, each update below the client's limit. */
	size_t budget = job->max_request_size > 4096
		? job->max_request_size - 1024 : 3072;
	size_t i = 0;
	while (i < job->n_bitmaps) {
		size_t n = 0, size = 0;
		while (i + n < job->n_bitmaps) {
			const struct rdp_encoded_bitmap* b = &job->bitmaps[i + n];
			size_t cost = b->length + 32;
			if (n > 0 && size + cost > budget)
				break;
			BITMAP_DATA* d = &data[n];
			memset(d, 0, sizeof(*d));
			d->destLeft = b->x;
			d->destTop = b->y;
			d->destRight = b->x + b->width - 1;
			d->destBottom = b->y + b->height - 1;
			d->width = b->width;
			d->height = b->height;
			d->bitsPerPixel = 32;
			d->compressed = TRUE;
			d->bitmapDataStream = b->data;
			d->bitmapLength = b->length;
			d->cbCompMainBodySize = b->length;
			d->cbScanWidth = b->width * 4;
			d->cbUncompressedSize = b->width * b->height * 4;
			size += cost;
			++n;
		}

		BITMAP_UPDATE bitmap_update = {
			.number = n,
			.rectangles = data,
		};
		if (!update->BitmapUpdate(context, &bitmap_update)) {
			wv_log(WV_LOG_ERROR, "Sending a bitmap update failed");
			free(data);
			rdp_peer_close(self);
			return;
		}
		self->bytes_sent += size;
		i += n;
	}
	free(data);
	self->frames_sent++;
}

void rdp_peer_send(struct rdp_peer* self, const struct rdp_encode_job* job)
{
	if (job->codec == RDP_CODEC_REMOTEFX)
		send_remotefx(self, job);
	else
		send_planar(self, job);
}
