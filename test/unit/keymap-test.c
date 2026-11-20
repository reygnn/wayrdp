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
#include "rdp/keymap.h"

static int test_plain_scancodes(void)
{
	ASSERT_UINT32_EQ(KEY_ESC, rdp_scancode_to_evdev(0x01, false));
	ASSERT_UINT32_EQ(KEY_A, rdp_scancode_to_evdev(0x1e, false));
	ASSERT_UINT32_EQ(KEY_Z, rdp_scancode_to_evdev(0x2c, false));
	ASSERT_UINT32_EQ(KEY_LEFTCTRL, rdp_scancode_to_evdev(0x1d, false));
	ASSERT_UINT32_EQ(KEY_LEFTALT, rdp_scancode_to_evdev(0x38, false));
	ASSERT_UINT32_EQ(KEY_CAPSLOCK, rdp_scancode_to_evdev(0x3a, false));
	ASSERT_UINT32_EQ(KEY_NUMLOCK, rdp_scancode_to_evdev(0x45, false));
	ASSERT_UINT32_EQ(KEY_KPDOT, rdp_scancode_to_evdev(0x53, false));
	/* The German <>| key and F11/F12 above the contiguous range. */
	ASSERT_UINT32_EQ(KEY_102ND, rdp_scancode_to_evdev(0x56, false));
	ASSERT_UINT32_EQ(KEY_F11, rdp_scancode_to_evdev(0x57, false));
	ASSERT_UINT32_EQ(KEY_F12, rdp_scancode_to_evdev(0x58, false));
	ASSERT_UINT32_EQ(0, rdp_scancode_to_evdev(0x00, false));
	ASSERT_UINT32_EQ(0, rdp_scancode_to_evdev(0xff, false));
	return 0;
}

static int test_extended_scancodes(void)
{
	ASSERT_UINT32_EQ(KEY_RIGHTALT, rdp_scancode_to_evdev(0x38, true));
	ASSERT_UINT32_EQ(KEY_RIGHTCTRL, rdp_scancode_to_evdev(0x1d, true));
	ASSERT_UINT32_EQ(KEY_KPENTER, rdp_scancode_to_evdev(0x1c, true));
	ASSERT_UINT32_EQ(KEY_KPSLASH, rdp_scancode_to_evdev(0x35, true));
	ASSERT_UINT32_EQ(KEY_UP, rdp_scancode_to_evdev(0x48, true));
	ASSERT_UINT32_EQ(KEY_DELETE, rdp_scancode_to_evdev(0x53, true));
	ASSERT_UINT32_EQ(KEY_LEFTMETA, rdp_scancode_to_evdev(0x5b, true));
	ASSERT_UINT32_EQ(KEY_COMPOSE, rdp_scancode_to_evdev(0x5d, true));
	ASSERT_UINT32_EQ(KEY_SYSRQ, rdp_scancode_to_evdev(0x37, true));
	ASSERT_UINT32_EQ(0, rdp_scancode_to_evdev(0x01, true));
	return 0;
}

static int test_key_decode(void)
{
	struct rdp_key_decoder dec = { 0 };
	uint32_t code = 0;
	bool pressed = false;

	ASSERT_TRUE(rdp_key_decode(&dec, 0, 0x1e, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_A, code);
	ASSERT_TRUE(pressed);

	ASSERT_TRUE(rdp_key_decode(&dec, RDP_KBD_RELEASE, 0x1e, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_A, code);
	ASSERT_FALSE(pressed);

	/* AltGr */
	ASSERT_TRUE(rdp_key_decode(&dec, RDP_KBD_EXTENDED, 0x38, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_RIGHTALT, code);

	/* Pause: E1 1D, then 45, down and up. */
	ASSERT_FALSE(rdp_key_decode(&dec, RDP_KBD_EXTENDED1, 0x1d, &code, &pressed));
	ASSERT_TRUE(rdp_key_decode(&dec, 0, 0x45, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_PAUSE, code);
	ASSERT_TRUE(pressed);
	ASSERT_FALSE(rdp_key_decode(&dec, RDP_KBD_EXTENDED1 | RDP_KBD_RELEASE,
				0x1d, &code, &pressed));
	ASSERT_TRUE(rdp_key_decode(&dec, RDP_KBD_RELEASE, 0x45, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_PAUSE, code);
	ASSERT_FALSE(pressed);

	/* The second half with E1 as well. */
	ASSERT_FALSE(rdp_key_decode(&dec, RDP_KBD_EXTENDED1, 0x1d, &code, &pressed));
	ASSERT_TRUE(rdp_key_decode(&dec, RDP_KBD_EXTENDED1, 0x45, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_PAUSE, code);

	/* 45 alone is NumLock. */
	ASSERT_TRUE(rdp_key_decode(&dec, 0, 0x45, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_NUMLOCK, code);

	/* E1 1D followed by something else: no Pause, the key goes through. */
	ASSERT_FALSE(rdp_key_decode(&dec, RDP_KBD_EXTENDED1, 0x1d, &code, &pressed));
	ASSERT_TRUE(rdp_key_decode(&dec, 0, 0x1e, &code, &pressed));
	ASSERT_UINT32_EQ(KEY_A, code);

	/* Unknown keys send nothing. */
	ASSERT_FALSE(rdp_key_decode(&dec, 0, 0x00, &code, &pressed));
	return 0;
}

static int test_wheel(void)
{
	/* One notch away from the user. */
	ASSERT_INT32_EQ(120, rdp_wheel_rotation(0x0200 | 0x0078));
	/* One notch towards the user: -120 as 9 bits is 0x188. */
	ASSERT_INT32_EQ(-120, rdp_wheel_rotation(0x0200 | 0x0188));
	/* High-resolution wheels send fractions. */
	ASSERT_INT32_EQ(15, rdp_wheel_rotation(0x0400 | 0x000f));
	ASSERT_INT32_EQ(-1, rdp_wheel_rotation(0x0200 | 0x01ff));
	ASSERT_INT32_EQ(-256, rdp_wheel_rotation(0x0200 | 0x0100));
	ASSERT_INT32_EQ(255, rdp_wheel_rotation(0x0200 | 0x00ff));
	return 0;
}

static int test_utf16(void)
{
	struct rdp_utf16_decoder dec = { 0 };
	ASSERT_UINT32_EQ('a', rdp_utf16_decode(&dec, 'a'));
	ASSERT_UINT32_EQ(0xe4, rdp_utf16_decode(&dec, 0xe4));      /* ä */
	ASSERT_UINT32_EQ(0x20ac, rdp_utf16_decode(&dec, 0x20ac));  /* € */
	/* U+1F600 as D83D DE00 */
	ASSERT_UINT32_EQ(0, rdp_utf16_decode(&dec, 0xd83d));
	ASSERT_UINT32_EQ(0x1f600, rdp_utf16_decode(&dec, 0xde00));
	/* A lone low surrogate, a high one followed by a plain unit. */
	ASSERT_UINT32_EQ(0, rdp_utf16_decode(&dec, 0xde00));
	ASSERT_UINT32_EQ(0, rdp_utf16_decode(&dec, 0xd83d));
	ASSERT_UINT32_EQ('b', rdp_utf16_decode(&dec, 'b'));
	ASSERT_UINT32_EQ(0, rdp_utf16_decode(&dec, 0xde00));
	return 0;
}

/* U+1F600 pressed and let go, each unit its own event: both the press
 * and the release come out, however the two streams interleave. */
static int test_unicode_keys(void)
{
	struct rdp_unicode_keys keys = { 0 };
	ASSERT_UINT32_EQ(0, rdp_unicode_key(&keys, 0xd83d, false));
	ASSERT_UINT32_EQ(0, rdp_unicode_key(&keys, 0xd83d, true));
	ASSERT_UINT32_EQ(0x1f600, rdp_unicode_key(&keys, 0xde00, false));
	ASSERT_UINT32_EQ(0x1f600, rdp_unicode_key(&keys, 0xde00, true));
	/* Down, down, up, up as well. */
	ASSERT_UINT32_EQ(0, rdp_unicode_key(&keys, 0xd83d, false));
	ASSERT_UINT32_EQ(0x1f600, rdp_unicode_key(&keys, 0xde00, false));
	ASSERT_UINT32_EQ(0, rdp_unicode_key(&keys, 0xd83d, true));
	ASSERT_UINT32_EQ(0x1f600, rdp_unicode_key(&keys, 0xde00, true));
	return 0;
}

int main(void)
{
	int r = 0;
	r |= test_plain_scancodes();
	r |= test_extended_scancodes();
	r |= test_key_decode();
	r |= test_wheel();
	r |= test_utf16();
	r |= test_unicode_keys();
	return r;
}
