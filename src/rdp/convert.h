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
 * Desktop sizes and pointer images, as RDP wants them. No FreeRDP in here
 * (test/convert-test.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MS-RDPEDISP allows 200 to 8192 a side, the width even. mwc takes at most
 * 8192x4320 pixels (a buffer of about 135 MiB). */
#define RDP_MIN_SIDE 200
#define RDP_MAX_SIDE 8192
#define RDP_MAX_AREA (8192u * 4320u)

/* The nearest size both take: clamped to the sides, the width rounded down
 * to even, the height lowered until the area fits. */
void rdp_clamp_size(uint32_t width, uint32_t height, uint32_t* out_width,
		uint32_t* out_height);

/*
 * A 32-bit pointer image for TS_COLORPOINTERATTRIBUTE / TS_LARGEPOINTER:
 * the XOR mask in BGRA with straight alpha, rows bottom-up; the AND mask
 * one bit per pixel, rows bottom-up and padded to 16 bits, set where the
 * pixel is fully transparent (and its XOR pixel is 0 there). Clients that
 * use the alpha and clients that use the mask both get the same picture.
 *
 * src: premultiplied ARGB8888 as wl_shm has it (B, G, R, A in memory),
 * rows top-down. xor_out needs width * height * 4 bytes, and_out
 * rdp_pointer_and_size().
 */
size_t rdp_pointer_and_size(uint32_t width, uint32_t height);
void rdp_pointer_convert(const uint8_t* src, uint32_t width, uint32_t height,
		uint32_t stride, uint8_t* xor_out, uint8_t* and_out);
