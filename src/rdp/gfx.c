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
 * Graphics pipeline (MS-RDPEGFX). mstsc offers RemoteFX only through it;
 * without it, it gets Planar bitmaps.
 *
 * The channel runs without a thread of its own: its event fd sits in aml's
 * main loop like the peer's, and what it sends goes through the channel
 * manager's queue, which on_peer_event() empties. So every callback here
 * runs on the main loop.
 *
 * Order: drdynvc ready -> open the channel -> the client advertises its
 * capabilities -> confirm one -> on each activation ResetGraphics, a new
 * surface mapped to the output -> frames of RemoteFX Progressive, each
 * acknowledged. A client that announced the pipeline gets no picture
 * until then; if the capabilities don't come in time, it gets the old way.
 */

#include <stdlib.h>
#include <string.h>
#include <aml.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/codec/color.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include "rdp/internal.h"
#include "log.h"

/* From opening the channel to the capabilities. */
#define CAPS_TIMEOUT_US 3000000

static const char* version_name(uint32_t version)
{
	switch (version) {
	case RDPGFX_CAPVERSION_8: return "8.0";
	case RDPGFX_CAPVERSION_81: return "8.1";
	case RDPGFX_CAPVERSION_10: return "10.0";
	case RDPGFX_CAPVERSION_101: return "10.1";
	case RDPGFX_CAPVERSION_102: return "10.2";
	case RDPGFX_CAPVERSION_103: return "10.3";
	case RDPGFX_CAPVERSION_104: return "10.4";
	case RDPGFX_CAPVERSION_105: return "10.5";
	case RDPGFX_CAPVERSION_106: return "10.6";
	case RDPGFX_CAPVERSION_106_ERR: return "10.6";
	case RDPGFX_CAPVERSION_107: return "10.7";
	}
	return "?";
}

/* The ones wayrdp speaks, best first. No H.264 (yet): AVC is switched
 * off in what is confirmed. */
static const uint32_t versions[] = {
	RDPGFX_CAPVERSION_107,
	RDPGFX_CAPVERSION_106,
	RDPGFX_CAPVERSION_106_ERR,
	RDPGFX_CAPVERSION_105,
	RDPGFX_CAPVERSION_104,
	RDPGFX_CAPVERSION_103,
	RDPGFX_CAPVERSION_102,
	RDPGFX_CAPVERSION_101,
	RDPGFX_CAPVERSION_10,
	RDPGFX_CAPVERSION_81,
	RDPGFX_CAPVERSION_8,
};

static bool is_v10(uint32_t version)
{
	return version != RDPGFX_CAPVERSION_8 && version != RDPGFX_CAPVERSION_81;
}

static void give_up(struct rdp_peer* self, const char* why)
{
	if (self->gfx_failed)
		return;
	wv_log(WV_LOG_INFO, "%s: no graphics pipeline (%s)",
			self->client->hostname, why);
	self->gfx_failed = true;
	self->gfx_ready = false;
	/* Not from inside the channel's own callbacks: on_gfx_event closes
	 * it once they return. */
	if (!self->gfx_dispatching)
		rdp_gfx_close(self);
	/* The old way now, from the start, if it is active. */
	if (self->activated && !self->closing)
		rdp_peer_reactivated(self);
}

static UINT on_caps_advertise(RdpgfxServerContext* context,
		const RDPGFX_CAPS_ADVERTISE_PDU* pdu)
{
	struct rdp_peer* self = context->custom;

	/* For the tests: a client that announced the pipeline and then sends
	 * no capabilities, so that the timeout and the old way get tested. */
	if (self->server->test.ignore_gfx_caps) {
		wv_log(WV_LOG_DEBUG, "%s: capabilities ignored (test)",
				self->client->hostname);
		return CHANNEL_RC_OK;
	}

	const RDPGFX_CAPSET* chosen = NULL;
	for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]) && !chosen;
			++v)
		for (UINT16 i = 0; i < pdu->capsSetCount; ++i)
			if (pdu->capsSets[i].version == versions[v]) {
				chosen = &pdu->capsSets[i];
				break;
			}

	if (!chosen) {
		give_up(self, "no capability version in common");
		return CHANNEL_RC_OK;
	}

	RDPGFX_CAPSET caps = *chosen;
	if (is_v10(caps.version))
		caps.flags |= RDPGFX_CAPS_FLAG_AVC_DISABLED;
	else
		caps.flags &= ~RDPGFX_CAPS_FLAG_AVC420_ENABLED;

	RDPGFX_CAPS_CONFIRM_PDU confirm = { .capsSet = &caps };
	UINT rc = context->CapsConfirm(context, &confirm);
	if (rc != CHANNEL_RC_OK) {
		give_up(self, "the confirmation failed");
		return CHANNEL_RC_OK;
	}

	if (self->gfx_timer)
		aml_stop(aml_get_default(), self->gfx_timer);
	self->gfx_ready = true;
	wv_log(WV_LOG_INFO, "%s: graphics pipeline %s, RemoteFX Progressive",
			self->client->hostname, version_name(caps.version));

	/* Waiting for this: start over with a surface and a whole picture. */
	if (self->activated)
		rdp_peer_reactivated(self);
	return CHANNEL_RC_OK;
}

static UINT on_frame_ack(RdpgfxServerContext* context,
		const RDPGFX_FRAME_ACKNOWLEDGE_PDU* pdu)
{
	struct rdp_peer* self = context->custom;
	if (pdu->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT
			&& rdp_flow_suspend(&self->flow))
		wv_log(WV_LOG_DEBUG, "%s: frame acknowledgements suspended",
				self->client->hostname);
	rdp_flow_ack(&self->flow, pdu->frameId);
	if (self->server->active == self)
		rdp_server_schedule(self->server);
	return CHANNEL_RC_OK;
}

static UINT on_qoe_frame_ack(RdpgfxServerContext* context,
		const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU* pdu)
{
	return CHANNEL_RC_OK;
}

static void on_gfx_event(struct aml_handler* handler)
{
	struct rdp_peer* self = aml_get_userdata(handler);
	if (self->closing || !self->gfx)
		return;

	self->gfx_dispatching = true;
	UINT rc = rdpgfx_server_handle_messages(self->gfx);
	self->gfx_dispatching = false;
	if (self->gfx_failed) {
		rdp_gfx_close(self);
		return;
	}
	if (rc != CHANNEL_RC_OK && rc != ERROR_NO_DATA) {
		wv_log(WV_LOG_WARNING, "%s: graphics pipeline error %u",
				self->client->hostname, rc);
		if (self->gfx_ready) {
			/* Its surface is gone with it. */
			rdp_peer_close(self);
			return;
		}
		give_up(self, "channel error");
	}
}

static void on_caps_timeout(struct aml_timer* timer)
{
	struct rdp_peer* self = aml_get_userdata(timer);
	if (!self->gfx_ready && !self->closing)
		give_up(self, "the client sent no capabilities");
}

/* Off for the tests of the old way (surface bits, or Planar). */
bool rdp_gfx_offered(const struct rdp_server* server)
{
	return !server->test.no_gfx && !server->test.force_planar;
}

bool rdp_gfx_pending(const struct rdp_peer* self)
{
	return !self->gfx_failed && !self->gfx_ready
		&& freerdp_settings_get_bool(self->client->context->settings,
				FreeRDP_SupportGraphicsPipeline);
}

void rdp_gfx_arm_timer(struct rdp_peer* self)
{
	if (self->gfx_timer || self->gfx_ready || self->gfx_failed)
		return;
	self->gfx_timer = aml_timer_new(CAPS_TIMEOUT_US, on_caps_timeout, self,
			NULL);
	if (!self->gfx_timer
			|| aml_start(aml_get_default(), self->gfx_timer) < 0)
		give_up(self, "no timer");
}

void rdp_gfx_check(struct rdp_peer* self)
{
	if (self->gfx_tried || self->gfx_failed || !self->logged_on)
		return;
	if (!freerdp_settings_get_bool(self->client->context->settings,
				FreeRDP_SupportGraphicsPipeline)) {
		self->gfx_failed = true;
		return;
	}

	struct rdp_peer_context* ctx =
		(struct rdp_peer_context*)self->client->context;
	if (WTSVirtualChannelManagerGetDrdynvcState(ctx->vcm)
			!= DRDYNVC_STATE_READY)
		return;
	self->gfx_tried = true;

	RdpgfxServerContext* g = rdpgfx_server_context_new(ctx->vcm);
	if (!g) {
		give_up(self, "out of memory");
		return;
	}
	g->custom = self;
	g->rdpcontext = self->client->context;
	g->CapsAdvertise = on_caps_advertise;
	g->FrameAcknowledge = on_frame_ack;
	g->QoeFrameAcknowledge = on_qoe_frame_ack;
	/* Our thread drives it (externalThread). */
	if (!g->Initialize || !g->Initialize(g, TRUE) || !g->Open(g)) {
		rdpgfx_server_context_free(g);
		give_up(self, "the channel didn't open");
		return;
	}
	self->gfx = g;

	int fd = GetEventFileDescriptor(rdpgfx_server_get_event_handle(g));
	self->gfx_handler = fd >= 0
		? aml_handler_new(fd, on_gfx_event, self, NULL) : NULL;
	if (!self->gfx_handler
			|| aml_start(aml_get_default(), self->gfx_handler) < 0) {
		give_up(self, "can't watch the channel");
		return;
	}
	/* The capabilities have until then, counted from now. */
	if (self->gfx_timer) {
		aml_stop(aml_get_default(), self->gfx_timer);
		aml_set_duration(self->gfx_timer, CAPS_TIMEOUT_US);
		aml_start(aml_get_default(), self->gfx_timer);
	} else {
		rdp_gfx_arm_timer(self);
	}
	wv_log(WV_LOG_DEBUG, "%s: graphics pipeline channel open",
			self->client->hostname);
}

void rdp_gfx_close(struct rdp_peer* self)
{
	if (self->gfx_timer) {
		aml_stop(aml_get_default(), self->gfx_timer);
		aml_unref(self->gfx_timer);
		self->gfx_timer = NULL;
	}
	if (self->gfx_handler) {
		aml_stop(aml_get_default(), self->gfx_handler);
		aml_unref(self->gfx_handler);
		self->gfx_handler = NULL;
	}
	if (self->gfx) {
		if (!self->gfx->Close(self->gfx))
			wv_log(WV_LOG_DEBUG, "Closing the graphics pipeline failed");
		rdpgfx_server_context_free(self->gfx);
		self->gfx = NULL;
	}
	self->gfx_ready = false;
	self->gfx_surface = false;
}

bool rdp_gfx_reset_surface(struct rdp_peer* self, uint32_t width,
		uint32_t height)
{
	RdpgfxServerContext* g = self->gfx;
	UINT rc;

	if (self->gfx_surface) {
		RDPGFX_DELETE_SURFACE_PDU del = { .surfaceId = self->gfx_surface_id };
		if ((rc = g->DeleteSurface(g, &del)) != CHANNEL_RC_OK)
			goto failure;
		self->gfx_surface_id++;
		self->gfx_surface = false;
	}

	MONITOR_DEF monitor = {
		.left = 0,
		.top = 0,
		.right = width - 1,
		.bottom = height - 1,
		.flags = MONITOR_PRIMARY,
	};
	RDPGFX_RESET_GRAPHICS_PDU reset = {
		.width = width,
		.height = height,
		.monitorCount = 1,
		.monitorDefArray = &monitor,
	};
	if ((rc = g->ResetGraphics(g, &reset)) != CHANNEL_RC_OK)
		goto failure;

	RDPGFX_CREATE_SURFACE_PDU create = {
		.surfaceId = self->gfx_surface_id,
		.width = width,
		.height = height,
		.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888,
	};
	if ((rc = g->CreateSurface(g, &create)) != CHANNEL_RC_OK)
		goto failure;
	self->gfx_surface = true;

	RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = {
		.surfaceId = self->gfx_surface_id,
		.outputOriginX = 0,
		.outputOriginY = 0,
	};
	if ((rc = g->MapSurfaceToOutput(g, &map)) != CHANNEL_RC_OK)
		goto failure;

	wv_log(WV_LOG_DEBUG, "%s: surface %u, %ux%u", self->client->hostname,
			self->gfx_surface_id, width, height);
	return true;

failure:
	wv_log(WV_LOG_ERROR, "%s: setting up the surface failed (%u)",
			self->client->hostname, rc);
	return false;
}

static uint32_t timestamp(void)
{
	SYSTEMTIME t;
	GetSystemTime(&t);
	return (uint32_t)t.wHour << 22 | (uint32_t)t.wMinute << 16
		| (uint32_t)t.wSecond << 10 | t.wMilliseconds;
}

bool rdp_gfx_send(struct rdp_peer* self, const struct rdp_encode_job* job)
{
	RdpgfxServerContext* g = self->gfx;
	if (!g || !self->gfx_surface || job->n_messages == 0)
		return true;

	wStream* s = job->messages[0];
	RDPGFX_SURFACE_COMMAND cmd = {
		.surfaceId = self->gfx_surface_id,
		.codecId = RDPGFX_CODECID_CAPROGRESSIVE,
		.format = PIXEL_FORMAT_BGRX32,
		.left = job->bounds.left,
		.top = job->bounds.top,
		.right = job->bounds.right,
		.bottom = job->bounds.bottom,
		.width = job->bounds.right - job->bounds.left,
		.height = job->bounds.bottom - job->bounds.top,
		.length = Stream_GetPosition(s),
		.data = Stream_Buffer(s),
	};
	RDPGFX_START_FRAME_PDU start = {
		.frameId = rdp_flow_next_id(&self->flow),
		.timestamp = timestamp(),
	};
	RDPGFX_END_FRAME_PDU end = { .frameId = start.frameId };

	UINT rc = g->SurfaceFrameCommand(g, &cmd, &start, &end);
	if (rc != CHANNEL_RC_OK) {
		wv_log(WV_LOG_ERROR, "%s: sending a frame failed (%u)",
				self->client->hostname, rc);
		return false;
	}
	self->bytes_sent += cmd.length;
	self->frames_sent++;
	return true;
}
