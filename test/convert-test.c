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

#include "tst.h"
#include "rdp/convert.h"

static int size(uint32_t w, uint32_t h, uint32_t want_w, uint32_t want_h)
{
	uint32_t ow, oh;
	rdp_clamp_size(w, h, &ow, &oh);
	if (ow != want_w || oh != want_h) {
		fprintf(stderr, "FAILED: %ux%u gave %ux%u, wanted %ux%u\n",
				w, h, ow, oh, want_w, want_h);
		return 1;
	}
	return 0;
}

static int test_clamp(void)
{
	int r = 0;
	r |= size(1920, 1080, 1920, 1080);
	r |= size(1366, 768, 1366, 768);
	/* Odd width rounded down. */
	r |= size(1025, 769, 1024, 769);
	/* Too small, too large. */
	r |= size(100, 50, 200, 200);
	r |= size(10000, 300, 8192, 300);
	/* 8192x4320 fits, 8192x8192 doesn't. */
	r |= size(8192, 4320, 8192, 4320);
	r |= size(8192, 8192, 8192, 4320);
	r |= size(7680, 7680, 7680, 4608);
	return r;
}

static int test_pointer(void)
{
	/* 3x2: opaque red, half-transparent premultiplied white, transparent;
	 * opaque blue, transparent, opaque green. */
	const uint8_t src[2][3 * 4 + 4] = {
		{ 0, 0, 255, 255,   128, 128, 128, 128,   9, 9, 9, 0 },
		{ 255, 0, 0, 255,   0, 0, 0, 0,   0, 255, 0, 255 },
	};
	uint8_t xor_out[3 * 2 * 4];
	uint8_t and_out[4];
	ASSERT_INT_EQ(4, (int)rdp_pointer_and_size(3, 2));
	rdp_pointer_convert(&src[0][0], 3, 2, sizeof(src[0]), xor_out, and_out);

	/* Bottom-up: the source's second row comes first. */
	const uint8_t want_xor[] = {
		255, 0, 0, 255,   0, 0, 0, 0,   0, 255, 0, 255,
		0, 0, 255, 255,   255, 255, 255, 128,   0, 0, 0, 0,
	};
	ASSERT_INT_EQ(0, memcmp(xor_out, want_xor, sizeof(want_xor)));
	/* Row 0 (source row 1): pixel 1 transparent. Row 1: pixel 2. Two bytes
	 * a row. */
	ASSERT_INT_EQ(0x40, and_out[0]);
	ASSERT_INT_EQ(0x00, and_out[1]);
	ASSERT_INT_EQ(0x20, and_out[2]);
	ASSERT_INT_EQ(0x00, and_out[3]);
	return 0;
}

static int test_and_padding(void)
{
	/* 17 pixels need 3 bytes, padded to 4. */
	ASSERT_INT_EQ(4 * 5, (int)rdp_pointer_and_size(17, 5));
	ASSERT_INT_EQ(2 * 24, (int)rdp_pointer_and_size(16, 24));
	return 0;
}

int main(void)
{
	int r = 0;
	r |= test_clamp();
	r |= test_pointer();
	r |= test_and_padding();
	return r;
}
