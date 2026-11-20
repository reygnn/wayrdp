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

#include "rdp/flow.h"

void rdp_flow_reset(struct rdp_flow* self, bool acks)
{
	*self = (struct rdp_flow){
		.acks = acks,
		.waiting = acks,
	};
}

void rdp_flow_continue(struct rdp_flow* self)
{
	self->acked = self->sent;
}

uint32_t rdp_flow_next_id(struct rdp_flow* self)
{
	return ++self->sent;
}

bool rdp_flow_ack(struct rdp_flow* self, uint32_t id)
{
	if ((int32_t)(id - self->acked) < 0 || (int32_t)(self->sent - id) < 0)
		return false;
	self->acked = id;
	self->ack_seen = true;
	return true;
}

bool rdp_flow_suspend(struct rdp_flow* self)
{
	bool was = self->waiting;
	self->waiting = false;
	if (was)
		self->suspended = true;
	return was;
}

uint32_t rdp_flow_in_flight(const struct rdp_flow* self)
{
	if (!self->acks)
		return 0;
	return self->sent - self->acked;
}

bool rdp_flow_must_wait(const struct rdp_flow* self)
{
	return self->waiting
		&& rdp_flow_in_flight(self) >= RDP_MAX_FRAMES_IN_FLIGHT;
}

enum rdp_flow_timeout rdp_flow_timeout(struct rdp_flow* self)
{
	if (!self->ack_seen) {
		self->waiting = false;
		return RDP_FLOW_NEVER_ACKED;
	}
	self->acked = self->sent;
	return RDP_FLOW_ACK_LOST;
}

bool rdp_flow_resume(struct rdp_flow* self)
{
	if (!self->suspended)
		return false;
	self->suspended = false;
	self->waiting = true;
	self->acked = self->sent;
	return true;
}
