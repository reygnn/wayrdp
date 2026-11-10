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
 * Display control (MS-RDPEDISP): the client sends a monitor layout when it
 * connects and whenever its window changes size; the output follows.
 *
 * FreeRDP's disp server reads the channel in a thread of its own and calls
 * DispMonitorLayout there. The callback only stores the size and pokes an
 * eventfd; the main loop takes it from there. While the window is being
 * dragged many layouts come, so the size has to settle for 200 ms first.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <aml.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/server/disp.h>

#include "rdp/internal.h"
#include "rdp/convert.h"
#include "log.h"

#define SETTLE_US 200000

/* FreeRDP's thread. */
static UINT on_monitor_layout(DispServerContext* context,
		const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU* pdu)
{
	struct rdp_peer* self = context->custom;
	if (pdu->NumMonitors == 0)
		return CHANNEL_RC_OK;

	/* One output: the primary monitor, or the first. */
	const DISPLAY_CONTROL_MONITOR_LAYOUT* m = &pdu->Monitors[0];
	for (UINT32 i = 0; i < pdu->NumMonitors; ++i)
		if (pdu->Monitors[i].Flags & DISPLAY_CONTROL_MONITOR_PRIMARY)
			m = &pdu->Monitors[i];

	pthread_mutex_lock(&self->disp_lock);
	self->disp_width = m->Width;
	self->disp_height = m->Height;
	pthread_mutex_unlock(&self->disp_lock);

	uint64_t one = 1;
	ssize_t n = write(self->disp_eventfd, &one, sizeof(one));
	(void)n;
	return CHANNEL_RC_OK;
}

static void on_settled(struct aml_timer* timer)
{
	struct rdp_peer* self = aml_get_userdata(timer);
	if (self->closing || self->server->active != self)
		return;

	pthread_mutex_lock(&self->disp_lock);
	uint32_t w = self->disp_width, h = self->disp_height;
	pthread_mutex_unlock(&self->disp_lock);

	uint32_t cw, ch;
	if (rdp_server_request_size(self->server, w, h, &cw, &ch))
		wv_log(WV_LOG_INFO, "%s: window is %ux%u, output follows: %ux%u",
				self->client->hostname, w, h, cw, ch);
}

static void on_layout(struct aml_handler* handler)
{
	struct rdp_peer* self = aml_get_userdata(handler);
	uint64_t count;
	ssize_t n = read(self->disp_eventfd, &count, sizeof(count));
	(void)n;

	/* Start the wait again: only the last of a burst counts. */
	aml_stop(aml_get_default(), self->disp_timer);
	aml_set_duration(self->disp_timer, SETTLE_US);
	aml_start(aml_get_default(), self->disp_timer);
}

void rdp_display_check(struct rdp_peer* self)
{
	if (self->disp_tried || !self->logged_on || !self->server->config.resize_output)
		return;

	struct rdp_peer_context* ctx =
		(struct rdp_peer_context*)self->client->context;
	if (WTSVirtualChannelManagerGetDrdynvcState(ctx->vcm)
			!= DRDYNVC_STATE_READY)
		return;
	self->disp_tried = true;

	pthread_mutex_init(&self->disp_lock, NULL);
	self->disp_lock_init = true;
	self->disp_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->disp_eventfd < 0)
		return;
	self->disp_handler = aml_handler_new(self->disp_eventfd, on_layout,
			self, NULL);
	self->disp_timer = aml_timer_new(SETTLE_US, on_settled, self, NULL);
	if (!self->disp_handler || !self->disp_timer
			|| aml_start(aml_get_default(), self->disp_handler) < 0) {
		rdp_display_close(self);
		return;
	}

	DispServerContext* d = disp_server_context_new(ctx->vcm);
	if (!d) {
		rdp_display_close(self);
		return;
	}
	d->custom = self;
	d->rdpcontext = self->client->context;
	d->MaxNumMonitors = 1;
	/* The area wayrdp takes, as factors of the largest monitor. */
	d->MaxMonitorAreaFactorA = RDP_MAX_SIDE;
	d->MaxMonitorAreaFactorB = RDP_MAX_AREA / RDP_MAX_SIDE;
	d->DispMonitorLayout = on_monitor_layout;

	if (d->Open(d) != CHANNEL_RC_OK) {
		wv_log(WV_LOG_INFO, "%s: no display control", self->client->hostname);
		disp_server_context_free(d);
		rdp_display_close(self);
		return;
	}
	self->disp = d;

	if (d->DisplayControlCaps(d) != CHANNEL_RC_OK) {
		wv_log(WV_LOG_WARNING, "Display control setup failed");
		rdp_display_close(self);
		return;
	}
	wv_log(WV_LOG_DEBUG, "%s: display control open", self->client->hostname);
}

void rdp_display_close(struct rdp_peer* self)
{
	if (self->disp) {
		/* Joins FreeRDP's thread: no callback after this. */
		self->disp->Close(self->disp);
		disp_server_context_free(self->disp);
		self->disp = NULL;
	}
	if (self->disp_timer) {
		aml_stop(aml_get_default(), self->disp_timer);
		aml_unref(self->disp_timer);
		self->disp_timer = NULL;
	}
	if (self->disp_handler) {
		aml_stop(aml_get_default(), self->disp_handler);
		aml_unref(self->disp_handler);
		self->disp_handler = NULL;
	}
	if (self->disp_eventfd > 0) {
		close(self->disp_eventfd);
		self->disp_eventfd = -1;
	}
	if (self->disp_lock_init)
		pthread_mutex_destroy(&self->disp_lock);
	self->disp_lock_init = false;
}
