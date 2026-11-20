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

#include <string.h>

#include "rdp/convert.h"

void rdp_clamp_size(uint32_t width, uint32_t height, uint32_t* out_width,
		uint32_t* out_height)
{
	if (width < RDP_MIN_SIDE) width = RDP_MIN_SIDE;
	if (width > RDP_MAX_SIDE) width = RDP_MAX_SIDE;
	if (height < RDP_MIN_SIDE) height = RDP_MIN_SIDE;
	if (height > RDP_MAX_SIDE) height = RDP_MAX_SIDE;
	width &= ~1u;

	if ((uint64_t)width * height > RDP_MAX_AREA)
		height = RDP_MAX_AREA / width;

	*out_width = width;
	*out_height = height;
}

size_t rdp_pointer_and_size(uint32_t width, uint32_t height)
{
	return (size_t)((width + 15) / 16) * 2 * height;
}

static uint8_t unpremultiply(uint8_t c, uint8_t a)
{
	unsigned v = (c * 255u + a / 2) / a;
	return v > 255 ? 255 : v;
}

void rdp_pointer_convert(const uint8_t* src, uint32_t width, uint32_t height,
		uint32_t stride, uint8_t* xor_out, uint8_t* and_out)
{
	size_t and_stride = (size_t)((width + 15) / 16) * 2;
	memset(and_out, 0, and_stride * height);

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* in = src + (size_t)y * stride;
		/* Bottom-up. */
		uint32_t row = height - 1 - y;
		uint8_t* out = xor_out + (size_t)row * width * 4;
		uint8_t* mask = and_out + (size_t)row * and_stride;

		for (uint32_t x = 0; x < width; ++x) {
			uint8_t b = in[4 * x], g = in[4 * x + 1],
				r = in[4 * x + 2], a = in[4 * x + 3];
			if (a == 0) {
				memset(out + 4 * x, 0, 4);
				mask[x / 8] |= 0x80 >> (x % 8);
				continue;
			}
			if (a < 255) {
				/* Premultiplied to straight, rounded. A channel above
				 * alpha (not valid, but source-over passes one through)
				 * saturates instead of wrapping. */
				b = unpremultiply(b, a);
				g = unpremultiply(g, a);
				r = unpremultiply(r, a);
			}
			out[4 * x] = b;
			out[4 * x + 1] = g;
			out[4 * x + 2] = r;
			out[4 * x + 3] = a;
		}
	}
}
