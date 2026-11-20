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


#include <stdint.h>
#include <stdlib.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <linux/input-event-codes.h>

#include "pointer.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "time-util.h"

/* A notch of a mouse wheel scrolls 15 in Wayland's units, as wev shows for a
 * real one (wayvnc's number). */
#define AXIS_STEP_PER_NOTCH 15

int pointer_init(struct pointer* self)
{
	zwlr_virtual_pointer_v1_axis_source(self->pointer,
					    WL_POINTER_AXIS_SOURCE_WHEEL);
	self->current_x = -1;
	self->current_y = -1;
	return 0;
}

void pointer_destroy(struct pointer* self)
{
	/* None without a seat (init_pointer). */
	if (!self->pointer)
		return;
	zwlr_virtual_pointer_v1_destroy(self->pointer);
}

void pointer_motion(struct pointer* self, double x, double y)
{
	if (!self->pointer)
		return;
	if (x == self->current_x && y == self->current_y)
		return;

	if (x < 0) x = 0;
	if (x > 1) x = 1;
	if (y < 0) y = 0;
	if (y > 1) y = 1;

	zwlr_virtual_pointer_v1_motion_absolute(self->pointer, gettime_ms(),
			x * UINT32_MAX, y * UINT32_MAX, UINT32_MAX, UINT32_MAX);

	self->current_x = x;
	self->current_y = y;
}

void pointer_button(struct pointer* self, uint32_t code, bool is_pressed)
{
	if (!self->pointer)
		return;
	zwlr_virtual_pointer_v1_button(self->pointer, gettime_ms(), code,
			is_pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			           : WL_POINTER_BUTTON_STATE_RELEASED);
}

void pointer_axis(struct pointer* self, enum pointer_axis axis,
		int32_t value120)
{
	if (!self->pointer)
		return;
	uint32_t t = gettime_ms();
	uint32_t wl_axis = axis == POINTER_AXIS_VERTICAL
		? WL_POINTER_AXIS_VERTICAL_SCROLL
		: WL_POINTER_AXIS_HORIZONTAL_SCROLL;

	int32_t total = self->axis_remainder[axis] + value120;
	int32_t notches = total / 120;
	self->axis_remainder[axis] = total % 120;

	/* The value always follows the wheel; the notch count only goes with
	 * it once a whole notch is full, so high-resolution wheels don't
	 * scroll twice. */
	wl_fixed_t value = wl_fixed_from_double(
			value120 * AXIS_STEP_PER_NOTCH / 120.0);
	if (notches != 0)
		zwlr_virtual_pointer_v1_axis_discrete(self->pointer, t, wl_axis,
				value, notches);
	else if (value120 != 0)
		zwlr_virtual_pointer_v1_axis(self->pointer, t, wl_axis, value);
}

void pointer_frame(struct pointer* self)
{
	if (!self->pointer)
		return;
	zwlr_virtual_pointer_v1_frame(self->pointer);
}
