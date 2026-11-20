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

#include <stdlib.h>

#include "tst.h"
#include "rdp/text.h"

/* UTF-16LE from code units. */
static size_t units(uint8_t* out, const uint16_t* u, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		out[2 * i] = u[i] & 0xff;
		out[2 * i + 1] = u[i] >> 8;
	}
	return 2 * n;
}

static int to_utf8(const uint16_t* u, size_t n, const char* want)
{
	uint8_t buf[256];
	size_t len = units(buf, u, n), out_len = 0;
	char* s = rdp_utf16le_to_utf8(buf, len, &out_len);
	ASSERT_TRUE(s);
	if (strcmp(s, want) != 0 || out_len != strlen(want)) {
		fprintf(stderr, "FAILED: got '%s' (%zu), wanted '%s'\n", s,
				out_len, want);
		free(s);
		return 1;
	}
	free(s);
	return 0;
}

static int to_utf16(const char* text, const uint16_t* want, size_t n)
{
	size_t len = 0;
	uint8_t* out = rdp_utf8_to_utf16le(text, strlen(text), &len);
	ASSERT_TRUE(out);
	uint8_t buf[256];
	size_t want_len = units(buf, want, n);
	if (len != want_len || memcmp(out, buf, len) != 0) {
		fprintf(stderr, "FAILED: '%s' gave %zu bytes, wanted %zu\n",
				text, len, want_len);
		free(out);
		return 1;
	}
	free(out);
	return 0;
}

static int test_to_utf8(void)
{
	int r = 0;
	r |= to_utf8((uint16_t[]){ 'H', 'i', 0 }, 3, "Hi");
	/* Without a NUL, up to the length. */
	r |= to_utf8((uint16_t[]){ 'a', 'b' }, 2, "ab");
	/* Stops at the first NUL. */
	r |= to_utf8((uint16_t[]){ 'a', 0, 'b' }, 3, "a");
	r |= to_utf8((uint16_t[]){ 0xe4, 0xf6, 0xfc, 0xdf, 0x20ac, 0 }, 6,
			"äöüß€");
	/* U+1F600 */
	r |= to_utf8((uint16_t[]){ 0xd83d, 0xde00, 0 }, 3, "\xf0\x9f\x98\x80");
	/* CRLF becomes LF, a lone CR stays. */
	r |= to_utf8((uint16_t[]){ 'a', '\r', '\n', 'b', '\r', 'c', 0 }, 7,
			"a\nb\rc");
	/* Broken surrogates */
	r |= to_utf8((uint16_t[]){ 0xd83d, 'x', 0 }, 3, "\xef\xbf\xbdx");
	r |= to_utf8((uint16_t[]){ 0xde00, 0 }, 2, "\xef\xbf\xbd");
	r |= to_utf8((uint16_t[]){ 0 }, 1, "");
	return r;
}

static int test_to_utf16(void)
{
	int r = 0;
	r |= to_utf16("Hi", (uint16_t[]){ 'H', 'i', 0 }, 3);
	r |= to_utf16("", (uint16_t[]){ 0 }, 1);
	r |= to_utf16("äöüß€", (uint16_t[]){ 0xe4, 0xf6, 0xfc, 0xdf, 0x20ac, 0 }, 6);
	r |= to_utf16("\xf0\x9f\x98\x80", (uint16_t[]){ 0xd83d, 0xde00, 0 }, 3);
	/* LF becomes CRLF, CRLF stays CRLF. */
	r |= to_utf16("a\nb\r\nc", (uint16_t[]){ 'a', '\r', '\n', 'b', '\r',
			'\n', 'c', 0 }, 8);
	/* Broken UTF-8: a lone continuation byte, a cut sequence, an overlong
	 * slash, an encoded surrogate. */
	r |= to_utf16("\x80x", (uint16_t[]){ 0xfffd, 'x', 0 }, 3);
	r |= to_utf16("x\xe2\x82", (uint16_t[]){ 'x', 0xfffd, 0xfffd, 0 }, 4);
	r |= to_utf16("\xc0\xaf", (uint16_t[]){ 0xfffd, 0xfffd, 0 }, 3);
	r |= to_utf16("\xed\xa0\x80", (uint16_t[]){ 0xfffd, 0xfffd, 0xfffd, 0 }, 4);
	return r;
}

static int test_round_trip(void)
{
	const char* text = "Grüße, Welt!\nZeile 2 – 😀\n";
	size_t len16 = 0, len8 = 0;
	uint8_t* u16 = rdp_utf8_to_utf16le(text, strlen(text), &len16);
	ASSERT_TRUE(u16);
	char* back = rdp_utf16le_to_utf8(u16, len16, &len8);
	ASSERT_TRUE(back);
	int ok = strcmp(back, text) == 0;
	free(u16);
	free(back);
	ASSERT_TRUE(ok);
	return 0;
}

int main(void)
{
	int r = 0;
	r |= test_to_utf8();
	r |= test_to_utf16();
	r |= test_round_trip();
	return r;
}
