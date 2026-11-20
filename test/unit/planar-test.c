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
 * The Planar encoder keeps one 64x64 context for all tiles of a frame.
 * Its output must be byte for byte what a context reset to each tile's
 * size gives (the way FreeRDP's own encoders use it), also for the
 * smaller tiles at the right and bottom edge, and across frames.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/color.h>

#include "tst.h"
#include "rdp/internal.h"
#include "buffer.h"
#include "log.h"

/* What encoder.c needs from the rest of wayrdp. */
void wv__log(enum wv_log_level level, const char* file, int line,
		const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void wv_buffer_release(struct wv_buffer* buffer)
{
}

#define W 200
#define H 130

static void fill(uint8_t* pixels, uint32_t seed)
{
	/* Flat areas, edges and noise, so RLE has both runs and literals. */
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			uint8_t* p = pixels + (size_t)y * W * 4 + (size_t)x * 4;
			seed = seed * 1103515245 + 12345;
			bool flat = ((x / 16) + (y / 16)) % 3 == 0;
			p[0] = flat ? 0x40 : (uint8_t)(seed >> 16);
			p[1] = flat ? 0x80 : (uint8_t)(x * 3 + y);
			p[2] = flat ? 0xc0 : (uint8_t)(seed >> 8);
			p[3] = 0xff;
		}
}

static int compare_frame(struct rdp_encoder* encoder, uint8_t* pixels)
{
	struct wv_buffer buffer = { .pixels = pixels };
	RFX_RECT whole = { 0, 0, W, H };
	struct rdp_encode_job job = {
		.codec = RDP_CODEC_PLANAR,
		.buffer = &buffer,
		.width = W,
		.height = H,
		.stride = W * 4,
		.pixel_format = PIXEL_FORMAT_BGRX32,
		.rects = &whole,
		.n_rects = 1,
	};
	rdp_encode(encoder, &job);
	ASSERT_TRUE(job.ok);
	/* 4 x 3 tiles: 64, 64, 64, 8 wide; 64, 64, 2 high. */
	ASSERT_INT_EQ(12, (int)job.n_bitmaps);

	BITMAP_PLANAR_CONTEXT* ref = freerdp_bitmap_planar_context_new(
			PLANAR_FORMAT_HEADER_RLE | PLANAR_FORMAT_HEADER_NA, 64, 64);
	ASSERT_TRUE(ref != NULL);

	int r = 0;
	for (size_t i = 0; i < job.n_bitmaps; ++i) {
		const struct rdp_encoded_bitmap* b = &job.bitmaps[i];
		ASSERT_TRUE(freerdp_bitmap_planar_context_reset(ref, b->width,
					b->height));
		uint32_t size = 0;
		uint8_t* want = freerdp_bitmap_compress_planar(ref,
				pixels + (size_t)b->y * W * 4 + (size_t)b->x * 4,
				PIXEL_FORMAT_BGRX32, b->width, b->height, W * 4,
				NULL, &size);
		ASSERT_TRUE(want != NULL);
		if (size != b->length || memcmp(want, b->data, size) != 0) {
			fprintf(stderr, "FAILED: tile %ux%u at %u,%u: %u bytes, wanted %u\n",
					b->width, b->height, b->x, b->y, b->length,
					size);
			r = 1;
		}
		free(want);
	}
	freerdp_bitmap_planar_context_free(ref);

	for (size_t i = 0; i < job.n_bitmaps; ++i)
		free(job.bitmaps[i].data);
	free(job.bitmaps);
	return r;
}

static int test_planar_tiles(void)
{
	struct rdp_encoder encoder;
	ASSERT_INT_EQ(0, rdp_encoder_init(&encoder));
	uint8_t* pixels = malloc((size_t)W * H * 4);
	ASSERT_TRUE(pixels != NULL);

	int r = 0;
	/* Two frames: the context carries over from one to the next. */
	fill(pixels, 1);
	r |= compare_frame(&encoder, pixels);
	fill(pixels, 2);
	r |= compare_frame(&encoder, pixels);

	free(pixels);
	rdp_encoder_fini(&encoder);
	return r;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_planar_tiles);
	return r;
}
