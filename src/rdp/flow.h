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
 * Flow control: how many frames may be on their way to the client.
 *
 * Frames carry ids the client acknowledges, either the frame markers of the
 * old way or the graphics pipeline's own. At most RDP_MAX_FRAMES_IN_FLIGHT
 * may be unacknowledged; then the server waits. The wait has a timeout
 * (server.c): until the first ack has come it means the client never
 * acknowledges, and the waiting is switched off; after that a missing ack
 * counts as lost (mstsc skips them while inactive). The pipeline can also
 * suspend acks. Clients that send no ids at all (Planar, surface bits
 * without frame markers) are never waited for.
 *
 * Plain state, no FreeRDP: test/unit/flow-test.c pins it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Frames sent but not acknowledged before the server waits. */
#define RDP_MAX_FRAMES_IN_FLIGHT 2

struct rdp_flow {
	/* Frames carry ids the client acknowledges. */
	bool acks;
	/* Wait for them (off when the client never acknowledges, or the
	 * pipeline suspended acks). */
	bool waiting;
	/* An ack has come since the reset. */
	bool ack_seen;
	/* The id of the last frame sent, and of the last one acknowledged. */
	uint32_t sent;
	uint32_t acked;
};

/* A new way of sending: ids from the start, waiting if they are acked. */
void rdp_flow_reset(struct rdp_flow* self, bool acks);
/* The same way of sending on a new surface (the pipeline after a size
 * change): ids go on, and what is still on its way counts as arrived, as
 * acks for the old surface may still come. */
void rdp_flow_continue(struct rdp_flow* self);
/* The id for the next frame. */
uint32_t rdp_flow_next_id(struct rdp_flow* self);
/* An ack counts only for a frame between the last one acknowledged and
 * the last one sent (ids wrap). One from before a reset, or a late one for
 * an older frame, would otherwise make the server wait for nothing. false
 * if it didn't count. */
bool rdp_flow_ack(struct rdp_flow* self, uint32_t id);
/* The client stops acknowledging (the pipeline's SUSPEND); true if it was
 * waited for until now. */
bool rdp_flow_suspend(struct rdp_flow* self);
uint32_t rdp_flow_in_flight(const struct rdp_flow* self);
/* No new frame now: too many are unacknowledged. */
bool rdp_flow_must_wait(const struct rdp_flow* self);

enum rdp_flow_timeout {
	/* No ack ever came: the client doesn't acknowledge; no more waiting. */
	RDP_FLOW_NEVER_ACKED,
	/* An ack is missing: what is on its way counts as arrived. */
	RDP_FLOW_ACK_LOST,
};
/* The wait for an ack timed out. */
enum rdp_flow_timeout rdp_flow_timeout(struct rdp_flow* self);
