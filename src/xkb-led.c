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


#include "xkb-led.h"

bool xkb_key_lights_led(struct xkb_keymap* keymap, xkb_keycode_t key,
		const char* led_name)
{
	if (xkb_keymap_led_get_index(keymap, led_name) == XKB_LED_INVALID)
		return false;
	struct xkb_state* state = xkb_state_new(keymap);
	if (!state)
		return false;
	xkb_state_update_key(state, key, XKB_KEY_DOWN);
	xkb_state_update_key(state, key, XKB_KEY_UP);
	bool lit = xkb_state_led_name_is_active(state, led_name) > 0;
	xkb_state_unref(state);
	return lit;
}

size_t xkb_locks_to_press(struct xkb_keymap* keymap, struct xkb_state* state,
		const struct xkb_lock* locks, size_t n, xkb_keycode_t* out)
{
	size_t count = 0;
	for (size_t i = 0; i < n; ++i) {
		/* A lock whose LED never lights would be pressed every time. */
		if (!xkb_key_lights_led(keymap, locks[i].key, locks[i].led_name))
			continue;
		bool have = xkb_state_led_name_is_active(state, locks[i].led_name) > 0;
		if (have != locks[i].want)
			out[count++] = locks[i].key;
	}
	return count;
}
