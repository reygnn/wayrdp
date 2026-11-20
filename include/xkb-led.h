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
 * Which lock keys a keymap can show: a lock is synced with the client only
 * where pressing its key lights its LED. xkeyboard-config's usual keymaps
 * give Scroll_Lock no modifier, so its LED never lights, and a sync would
 * press the key again on every Synchronize.
 *
 * Only xkbcommon: test/unit/xkb-led-test.c pins it.
 */

#pragma once

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

/* Whether a press and release of key (an xkb keycode) turns the LED
 * led_name on, from a state with all LEDs off. */
bool xkb_key_lights_led(struct xkb_keymap* keymap, xkb_keycode_t key,
		const char* led_name);

struct xkb_lock {
	xkb_keycode_t key;
	const char* led_name;
	/* On at the client. */
	bool want;
};

/* The lock keys to press once so the session's locks (state) are the
 * client's: those that differ, of the ones whose LED the keymap shows.
 * Writes at most n keys to out; returns how many. */
size_t xkb_locks_to_press(struct xkb_keymap* keymap, struct xkb_state* state,
		const struct xkb_lock* locks, size_t n, xkb_keycode_t* out);
