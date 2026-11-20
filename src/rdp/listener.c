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
#include <aml.h>
#include <freerdp/listener.h>
#include <winpr/synch.h>

#include "rdp/internal.h"
#include "log.h"
#include "time-util.h"

static void on_listener_event(struct aml_handler* handler)
{
	struct rdp_server* self = aml_get_userdata(handler);
	if (!self->listener->CheckFileDescriptor(self->listener))
		wv_log(WV_LOG_WARNING, "Accepting a connection failed");
}

/* Connections not logged on yet that may wait at a time. */
#define MAX_PENDING 4

/* Before accept: refuse while locked out after failed logins, or while
 * too many connections wait for their logon. */
static BOOL check_accept(freerdp_listener* listener)
{
	struct rdp_server* self = listener->info;
	if (rdp_auth_is_locked(&self->auth, gettime_us())) {
		wv_log(WV_LOG_WARNING, "Connection refused: locked after failed logins");
		return FALSE;
	}

	/* Every PAM check counts, also one whose client hung up, and every
	 * connection waiting for its logon without one. */
	int pending = self->pam_jobs;
	struct rdp_peer* peer;
	LIST_FOREACH(peer, &self->peers, link)
		if (!peer->logged_on && !peer->closing && !peer->pam_pending)
			++pending;
	if (pending >= MAX_PENDING) {
		wv_log(WV_LOG_WARNING, "Connection refused: %d are logging on already",
				pending);
		return FALSE;
	}
	return TRUE;
}

static BOOL on_peer_accepted(freerdp_listener* listener, freerdp_peer* client)
{
	struct rdp_server* self = listener->info;
	return rdp_peer_new(self, client) != NULL;
}

int rdp_listener_open(struct rdp_server* self)
{
	self->listener = freerdp_listener_new();
	if (!self->listener)
		return -1;

	self->listener->info = self;
	self->listener->PeerAccepted = on_peer_accepted;
	self->listener->CheckPeerAcceptRestrictions = check_accept;

	if (!self->listener->Open(self->listener, self->config.address,
				self->config.port)) {
		wv_log(WV_LOG_ERROR, "Can't listen on %s:%u", self->config.address,
				self->config.port);
		return -1;
	}

	HANDLE handles[8];
	DWORD n = self->listener->GetEventHandles(self->listener, handles, 8);
	if (n == 0) {
		wv_log(WV_LOG_ERROR, "The listener has no event handles");
		return -1;
	}

	for (DWORD i = 0; i < n; ++i) {
		int fd = GetEventFileDescriptor(handles[i]);
		if (fd < 0)
			continue;
		struct aml_handler* handler = aml_handler_new(fd,
				on_listener_event, self, NULL);
		if (!handler || aml_start(aml_get_default(), handler) < 0) {
			wv_log(WV_LOG_ERROR, "Can't watch the listener");
			aml_unref(handler);
			return -1;
		}
		self->listen_handlers[self->n_listen_handlers++] = handler;
	}

	wv_log(WV_LOG_INFO, "Listening on %s:%u", self->config.address,
			self->config.port);
	return 0;
}

void rdp_listener_close(struct rdp_server* self)
{
	for (int i = 0; i < self->n_listen_handlers; ++i) {
		aml_stop(aml_get_default(), self->listen_handlers[i]);
		aml_unref(self->listen_handlers[i]);
	}
	self->n_listen_handlers = 0;

	if (self->listener) {
		self->listener->Close(self->listener);
		freerdp_listener_free(self->listener);
		self->listener = NULL;
	}
}
