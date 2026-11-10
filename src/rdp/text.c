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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "rdp/text.h"

#define REPLACEMENT 0xfffd

static size_t put_utf8(char* out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = 0xc0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = 0xe0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3f);
		out[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | (cp >> 18);
	out[1] = 0x80 | ((cp >> 12) & 0x3f);
	out[2] = 0x80 | ((cp >> 6) & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	return 4;
}

char* rdp_utf16le_to_utf8(const uint8_t* data, size_t len, size_t* out_len)
{
	size_t units = len / 2;
	/* At most three bytes of UTF-8 per UTF-16 unit. */
	char* out = malloc(units * 3 + 1);
	if (!out)
		return NULL;

	size_t n = 0;
	for (size_t i = 0; i < units; ++i) {
		uint32_t u = data[2 * i] | (data[2 * i + 1] << 8);
		if (u == 0)
			break;

		uint32_t cp = u;
		if (u >= 0xd800 && u <= 0xdbff) {
			uint32_t low = i + 1 < units
				? (uint32_t)(data[2 * i + 2] | (data[2 * i + 3] << 8))
				: 0;
			if (low >= 0xdc00 && low <= 0xdfff) {
				cp = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
				++i;
			} else {
				cp = REPLACEMENT;
			}
		} else if (u >= 0xdc00 && u <= 0xdfff) {
			cp = REPLACEMENT;
		}

		/* CRLF to LF. */
		if (cp == '\n' && n > 0 && out[n - 1] == '\r')
			--n;
		n += put_utf8(out + n, cp);
	}

	out[n] = '\0';
	*out_len = n;
	return out;
}

/* One code point from UTF-8; *pos moves past it. Overlong forms,
 * surrogates and broken sequences give U+FFFD and move one byte. */
static uint32_t next_utf8(const uint8_t* s, size_t len, size_t* pos)
{
	size_t i = *pos;
	uint8_t c = s[i];
	int extra;
	uint32_t cp, min;

	if (c < 0x80) {
		*pos = i + 1;
		return c;
	} else if ((c & 0xe0) == 0xc0) {
		extra = 1; cp = c & 0x1f; min = 0x80;
	} else if ((c & 0xf0) == 0xe0) {
		extra = 2; cp = c & 0x0f; min = 0x800;
	} else if ((c & 0xf8) == 0xf0) {
		extra = 3; cp = c & 0x07; min = 0x10000;
	} else {
		*pos = i + 1;
		return REPLACEMENT;
	}

	if (i + extra >= len) {
		*pos = i + 1;
		return REPLACEMENT;
	}
	for (int k = 1; k <= extra; ++k) {
		if ((s[i + k] & 0xc0) != 0x80) {
			*pos = i + 1;
			return REPLACEMENT;
		}
		cp = (cp << 6) | (s[i + k] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
		*pos = i + 1;
		return REPLACEMENT;
	}
	*pos = i + 1 + extra;
	return cp;
}

uint8_t* rdp_utf8_to_utf16le(const char* text, size_t len, size_t* out_len)
{
	const uint8_t* s = (const uint8_t*)text;
	/* Each byte gives at most one unit, LF two (CRLF); a 4-byte
	 * sequence gives two units. Plus the NUL. */
	size_t cap = (2 * len + 1) * 2;
	uint8_t* out = malloc(cap);
	if (!out)
		return NULL;

	size_t n = 0;
	uint32_t prev = 0;
	size_t pos = 0;
	while (pos < len) {
		uint32_t cp = next_utf8(s, len, &pos);
		if (cp == 0)
			break;
		if (cp == '\n' && prev != '\r') {
			out[n++] = '\r';
			out[n++] = 0;
		}
		if (cp >= 0x10000) {
			uint32_t v = cp - 0x10000;
			uint32_t high = 0xd800 + (v >> 10), low = 0xdc00 + (v & 0x3ff);
			out[n++] = high & 0xff; out[n++] = high >> 8;
			out[n++] = low & 0xff; out[n++] = low >> 8;
		} else {
			out[n++] = cp & 0xff;
			out[n++] = cp >> 8;
		}
		prev = cp;
	}
	out[n++] = 0;
	out[n++] = 0;
	*out_len = n;
	return out;
}
