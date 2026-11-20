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


#include <linux/input-event-codes.h>

#include "tst.h"
#include "xkb-led.h"

static struct xkb_keymap* keymap(struct xkb_context* ctx, const char* layout)
{
	struct xkb_rule_names names = { .layout = layout };
	return xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

/* Caps and Num Lock light their LEDs; Scroll Lock doesn't (no modifier in
 * xkeyboard-config), so it mustn't be synced. */
static int test_layouts(void)
{
	struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	ASSERT_TRUE(ctx != NULL);
	const char* layouts[] = { "us", "de", "ch" };
	for (size_t i = 0; i < sizeof(layouts) / sizeof(*layouts); ++i) {
		struct xkb_keymap* km = keymap(ctx, layouts[i]);
		ASSERT_TRUE(km != NULL);
		ASSERT_TRUE(xkb_key_lights_led(km, KEY_CAPSLOCK + 8, XKB_LED_NAME_CAPS));
		ASSERT_TRUE(xkb_key_lights_led(km, KEY_NUMLOCK + 8, XKB_LED_NAME_NUM));
		ASSERT_FALSE(xkb_key_lights_led(km, KEY_SCROLLLOCK + 8, XKB_LED_NAME_SCROLL));
		/* Another key lights none. */
		ASSERT_FALSE(xkb_key_lights_led(km, KEY_A + 8, XKB_LED_NAME_CAPS));
		xkb_keymap_unref(km);
	}
	xkb_context_unref(ctx);
	return 0;
}

/* The client has Caps and Scroll Lock on, the session nothing: only Caps
 * Lock is pressed. Then the session has it: nothing. */
static int test_to_press(void)
{
	struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap* km = keymap(ctx, "de");
	ASSERT_TRUE(km != NULL);
	struct xkb_state* st = xkb_state_new(km);
	struct xkb_lock locks[] = {
		{ KEY_CAPSLOCK + 8, XKB_LED_NAME_CAPS, true },
		{ KEY_NUMLOCK + 8, XKB_LED_NAME_NUM, false },
		{ KEY_SCROLLLOCK + 8, XKB_LED_NAME_SCROLL, true },
	};
	xkb_keycode_t out[3];
	ASSERT_INT_EQ(1, (int)xkb_locks_to_press(km, st, locks, 3, out));
	ASSERT_UINT32_EQ(KEY_CAPSLOCK + 8, out[0]);
	xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
	xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_UP);
	ASSERT_INT_EQ(0, (int)xkb_locks_to_press(km, st, locks, 3, out));
	xkb_state_unref(st);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_layouts);
	RUN_TEST(test_to_press);
	return r;
}
