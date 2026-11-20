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
 * RDP input as Linux sees it: scancodes to evdev codes, wheel values, UTF-16
 * from the Unicode keyboard events. No FreeRDP in here, so the unit tests
 * can pin it (test/unit/keymap-test.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* MS-RDPBCGR 2.2.8.1.1.3.1.1.1, as FreeRDP passes them on (also for fast
 * path input). */
#define RDP_KBD_EXTENDED 0x0100
#define RDP_KBD_EXTENDED1 0x0200
#define RDP_KBD_RELEASE 0x8000

/* 2.2.8.1.1.3.1.1.5 */
#define RDP_SYNC_SCROLL_LOCK 0x01
#define RDP_SYNC_NUM_LOCK 0x02
#define RDP_SYNC_CAPS_LOCK 0x04

/* 2.2.8.1.1.3.1.1.3 */
#define RDP_PTR_WHEEL_NEGATIVE 0x0100
#define RDP_PTR_WHEEL_MASK 0x01ff

/* The evdev code (KEY_*) of a set-1 scancode, 0 if there is none. */
uint32_t rdp_scancode_to_evdev(uint8_t scancode, bool extended);

/* Pause comes as E1 1D 45 (press) and E1 9D C5 (release), in RDP as a key
 * event with EXTENDED1 for 0x1D followed by one for 0x45. */
struct rdp_key_decoder {
	bool pause_pending;
};

/* Turns one keyboard event into an evdev code and a direction. False if
 * nothing is to be sent (the first half of Pause, unknown keys). */
bool rdp_key_decode(struct rdp_key_decoder* self, uint16_t flags,
		uint8_t code, uint32_t* evdev, bool* pressed);

/* The signed rotation of a wheel event: 120 is one notch, positive is away
 * from the user (up) or to the right. */
int32_t rdp_wheel_rotation(uint16_t flags);

/* UTF-16 code units, possibly surrogate pairs, to code points. */
struct rdp_utf16_decoder {
	uint16_t high;
};

/* Returns the code point, or 0 while waiting for the second half of a pair
 * (and for lone or broken halves). */
uint32_t rdp_utf16_decode(struct rdp_utf16_decoder* self, uint16_t unit);

/* Unicode key events: presses and releases each come as UTF-16 units, a
 * pair as two events each, and the two streams may interleave (H down,
 * H up, L down, L up). One decoder for each, so neither eats a half of
 * the other's pair. */
struct rdp_unicode_keys {
	struct rdp_utf16_decoder press, release;
};

/* The code point once a unit completes it, else 0. */
uint32_t rdp_unicode_key(struct rdp_unicode_keys* self, uint16_t unit,
		bool release);
