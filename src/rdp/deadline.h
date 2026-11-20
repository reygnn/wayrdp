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
 * A deadline on its own thread: fire(arg) runs there once the time is up,
 * unless the deadline is stopped before. For what the main loop can't do
 * itself because it may be the thing that is stuck: FreeRDP accepts TLS
 * (and NLA) blocking, without a timeout, so a client that stops in the
 * middle holds the loop, and no aml timer of the loop runs. fire must be
 * safe to call from another thread (SetEvent is).
 *
 * No FreeRDP: test/unit/deadline-test.c pins it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct rdp_deadline;

/* NULL if the thread can't be made. */
struct rdp_deadline* rdp_deadline_start(uint64_t timeout_us,
		void (*fire)(void* arg), void* arg);

/* Stops the deadline at once (it doesn't wait for the time) and frees it.
 * True if fire ran. NULL is fine. */
bool rdp_deadline_stop(struct rdp_deadline* self);
