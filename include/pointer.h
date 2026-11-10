/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
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


#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "wlr-virtual-pointer-unstable-v1.h"

/*
 * The pointer as RDP sees it: an absolute position, single buttons and
 * wheels in 120ths of a notch. wayvnc's version took VNC button masks.
 */

enum pointer_axis {
	POINTER_AXIS_VERTICAL = 0,
	POINTER_AXIS_HORIZONTAL,
};

struct pointer {
	struct zwlr_virtual_pointer_v1* pointer;

	/* Last position, normalised to 0..1. */
	double current_x;
	double current_y;

	/* Remainders of high-resolution wheels that don't make a notch yet. */
	int32_t axis_remainder[2];
};

int pointer_init(struct pointer* self);
void pointer_destroy(struct pointer* self);

/* x and y from 0 to 1 over the captured output. */
void pointer_motion(struct pointer* self, double x, double y);
/* code: BTN_LEFT, BTN_RIGHT, … from linux/input-event-codes.h */
void pointer_button(struct pointer* self, uint32_t code, bool is_pressed);
/* value120: 120 is one notch. Positive is down or right, as in Wayland. */
void pointer_axis(struct pointer* self, enum pointer_axis axis,
		int32_t value120);
/* Ends a group of the calls above, like wl_pointer.frame. */
void pointer_frame(struct pointer* self);
