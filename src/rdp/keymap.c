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

#include <stddef.h>
#include <linux/input-event-codes.h>

#include "rdp/keymap.h"

/*
 * Set 1 scancodes without prefix. 0x01 to 0x53 are the evdev codes
 * themselves (Linux numbered its keys after the XT keyboard); above them the
 * table follows the kernel's atkbd and hyperv-keyboard mappings.
 */
static uint32_t plain_above_0x53(uint8_t code)
{
	switch (code) {
	case 0x54: return KEY_SYSRQ;        /* Alt+PrintScreen */
	case 0x56: return KEY_102ND;        /* <> on ISO keyboards */
	case 0x57: return KEY_F11;
	case 0x58: return KEY_F12;
	case 0x59: return KEY_KPEQUAL;
	case 0x5c: return KEY_KPJPCOMMA;
	case 0x64: return KEY_F13;
	case 0x65: return KEY_F14;
	case 0x66: return KEY_F15;
	case 0x67: return KEY_F16;
	case 0x68: return KEY_F17;
	case 0x69: return KEY_F18;
	case 0x6a: return KEY_F19;
	case 0x6b: return KEY_F20;
	case 0x6c: return KEY_F21;
	case 0x6d: return KEY_F22;
	case 0x6e: return KEY_F23;
	case 0x70: return KEY_KATAKANAHIRAGANA;
	case 0x71: return KEY_HANJA;        /* Korean keyboards send 0xF1/0xF2, */
	case 0x72: return KEY_HANGEUL;      /* Windows passes them as 0x71/0x72 */
	case 0x73: return KEY_RO;
	case 0x76: return KEY_F24;
	case 0x77: return KEY_HIRAGANA;
	case 0x78: return KEY_KATAKANA;
	case 0x79: return KEY_HENKAN;
	case 0x7b: return KEY_MUHENKAN;
	case 0x7d: return KEY_YEN;
	case 0x7e: return KEY_KPCOMMA;
	}
	return 0;
}

/* With the E0 prefix. */
static uint32_t extended(uint8_t code)
{
	switch (code) {
	case 0x10: return KEY_PREVIOUSSONG;
	case 0x19: return KEY_NEXTSONG;
	case 0x1c: return KEY_KPENTER;
	case 0x1d: return KEY_RIGHTCTRL;
	case 0x20: return KEY_MUTE;
	case 0x21: return KEY_CALC;
	case 0x22: return KEY_PLAYPAUSE;
	case 0x24: return KEY_STOPCD;
	case 0x2e: return KEY_VOLUMEDOWN;
	case 0x30: return KEY_VOLUMEUP;
	case 0x32: return KEY_HOMEPAGE;
	case 0x35: return KEY_KPSLASH;
	case 0x37: return KEY_SYSRQ;        /* PrintScreen */
	case 0x38: return KEY_RIGHTALT;     /* AltGr */
	case 0x46: return KEY_PAUSE;        /* Ctrl+Break */
	case 0x47: return KEY_HOME;
	case 0x48: return KEY_UP;
	case 0x49: return KEY_PAGEUP;
	case 0x4b: return KEY_LEFT;
	case 0x4d: return KEY_RIGHT;
	case 0x4f: return KEY_END;
	case 0x50: return KEY_DOWN;
	case 0x51: return KEY_PAGEDOWN;
	case 0x52: return KEY_INSERT;
	case 0x53: return KEY_DELETE;
	case 0x5b: return KEY_LEFTMETA;
	case 0x5c: return KEY_RIGHTMETA;
	case 0x5d: return KEY_COMPOSE;      /* the menu key */
	case 0x5e: return KEY_POWER;
	case 0x5f: return KEY_SLEEP;
	case 0x63: return KEY_WAKEUP;
	case 0x65: return KEY_SEARCH;
	case 0x66: return KEY_BOOKMARKS;
	case 0x67: return KEY_REFRESH;
	case 0x68: return KEY_STOP;
	case 0x69: return KEY_FORWARD;
	case 0x6a: return KEY_BACK;
	case 0x6b: return KEY_COMPUTER;
	case 0x6c: return KEY_MAIL;
	case 0x6d: return KEY_MEDIA;
	}
	return 0;
}

uint32_t rdp_scancode_to_evdev(uint8_t scancode, bool is_extended)
{
	if (is_extended)
		return extended(scancode);
	if (scancode >= 0x01 && scancode <= 0x53)
		return scancode;
	return plain_above_0x53(scancode);
}

bool rdp_key_decode(struct rdp_key_decoder* self, uint16_t flags,
		uint8_t code, uint32_t* evdev, bool* pressed)
{
	*pressed = !(flags & RDP_KBD_RELEASE);

	if (flags & RDP_KBD_EXTENDED1) {
		/* E1 1D: the first half of Pause. Some clients mark the second
		 * half (45) with E1 as well. Anything else with E1 is unknown. */
		if (code == 0x1d) {
			self->pause_pending = true;
			return false;
		}
		bool pause = self->pause_pending && code == 0x45;
		self->pause_pending = false;
		if (pause)
			*evdev = KEY_PAUSE;
		return pause;
	}

	if (self->pause_pending) {
		self->pause_pending = false;
		if (code == 0x45) {
			*evdev = KEY_PAUSE;
			return true;
		}
	}

	*evdev = rdp_scancode_to_evdev(code, flags & RDP_KBD_EXTENDED);
	return *evdev != 0;
}

int32_t rdp_wheel_rotation(uint16_t flags)
{
	/* Nine bits, two's complement: the sign bit is WHEEL_NEGATIVE. */
	int32_t value = flags & RDP_PTR_WHEEL_MASK;
	if (flags & RDP_PTR_WHEEL_NEGATIVE)
		value -= 0x200;
	return value;
}

uint32_t rdp_utf16_decode(struct rdp_utf16_decoder* self, uint16_t unit)
{
	if (unit >= 0xd800 && unit <= 0xdbff) {
		self->high = unit;
		return 0;
	}
	if (unit >= 0xdc00 && unit <= 0xdfff) {
		uint16_t high = self->high;
		self->high = 0;
		if (!high)
			return 0;
		return 0x10000 + (((uint32_t)high - 0xd800) << 10)
			+ (unit - 0xdc00);
	}
	self->high = 0;
	return unit;
}
