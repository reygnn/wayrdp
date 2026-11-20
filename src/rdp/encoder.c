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
 * Runs in aml's worker thread: touches only the encoder's codec contexts and
 * the job. The buffer stays referenced by the job, so the capture can't
 * reuse it meanwhile.
 */

#include <stdlib.h>
#include <string.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/region.h>
#include <freerdp/constants.h>
#include <winpr/stream.h>

#include "rdp/internal.h"
#include "buffer.h"
#include "log.h"
#include "time-util.h"

#define TILE 64

int rdp_encoder_init(struct rdp_encoder* self)
{
	memset(self, 0, sizeof(*self));

	/* RemoteFX spreads its tiles over WinPR's thread pool. */
	self->rfx = rfx_context_new_ex(TRUE, 0);
	self->planar = freerdp_bitmap_planar_context_new(
			PLANAR_FORMAT_HEADER_RLE | PLANAR_FORMAT_HEADER_NA,
			TILE, TILE);
	self->progressive = progressive_context_new(TRUE);
	if (!self->rfx || !self->planar || !self->progressive) {
		wv_log(WV_LOG_ERROR, "Can't create the codec contexts");
		rdp_encoder_fini(self);
		return -1;
	}
	return 0;
}

void rdp_encoder_fini(struct rdp_encoder* self)
{
	if (self->rfx)
		rfx_context_free(self->rfx);
	if (self->planar)
		freerdp_bitmap_planar_context_free(self->planar);
	if (self->progressive)
		progressive_context_free(self->progressive);
	self->rfx = NULL;
	self->planar = NULL;
	self->progressive = NULL;
}

/* RemoteFX tiles are 64×64 and compress to well under this; with it, a
 * group of tiles stays below the client's request size. */
#define RFX_TILE_BOUND (24 * 1024)

/* The 64×64 tiles the damage touches, each once, as rectangles clipped to
 * the desktop, in rows from the top. */
static int damaged_tiles(const struct rdp_encode_job* job, RFX_RECT** out)
{
	uint32_t cols = (job->width + TILE - 1) / TILE;
	uint32_t rows = (job->height + TILE - 1) / TILE;
	uint8_t* touched = calloc(cols * rows, 1);
	if (!touched)
		return -1;

	int n = 0;
	for (int i = 0; i < job->n_rects; ++i) {
		const RFX_RECT* r = &job->rects[i];
		if (r->width == 0 || r->height == 0)
			continue;
		uint32_t tx1 = (r->x + r->width - 1) / TILE;
		uint32_t ty1 = (r->y + r->height - 1) / TILE;
		for (uint32_t ty = r->y / TILE; ty <= ty1 && ty < rows; ++ty)
			for (uint32_t tx = r->x / TILE; tx <= tx1 && tx < cols; ++tx)
				if (!touched[ty * cols + tx]) {
					touched[ty * cols + tx] = 1;
					++n;
				}
	}

	RFX_RECT* tiles = calloc(n ? n : 1, sizeof(*tiles));
	if (!tiles) {
		free(touched);
		return -1;
	}

	int k = 0;
	for (uint32_t ty = 0; ty < rows; ++ty)
		for (uint32_t tx = 0; tx < cols; ++tx) {
			if (!touched[ty * cols + tx])
				continue;
			RFX_RECT* t = &tiles[k++];
			t->x = tx * TILE;
			t->y = ty * TILE;
			t->width = job->width - t->x < TILE ? job->width - t->x : TILE;
			t->height = job->height - t->y < TILE ? job->height - t->y : TILE;
		}

	free(touched);
	*out = tiles;
	return n;
}

/*
 * One message per group of tiles. Not rfx_encode_messages(): its split
 * leaks the message array in FreeRDP 3.32 (rfx_message_list_free() frees
 * the messages, not the array).
 */
static bool encode_remotefx(struct rdp_encoder* self,
		struct rdp_encode_job* job)
{
	if (job->reset || self->width != job->width
			|| self->height != job->height) {
		if (!rfx_context_reset(self->rfx, job->width, job->height))
			return false;
		self->width = job->width;
		self->height = job->height;
	}
	if (!rfx_context_set_mode(self->rfx, job->rlgr_mode))
		return false;
	rfx_context_set_pixel_format(self->rfx, job->pixel_format);

	RFX_RECT* tiles = NULL;
	int n_tiles = damaged_tiles(job, &tiles);
	if (n_tiles < 0)
		return false;

	size_t limit = job->max_request_size > 8192
		? job->max_request_size - 4096 : 4096;
	int per_message = limit / RFX_TILE_BOUND;
	if (per_message < 1)
		per_message = 1;

	int n_groups = (n_tiles + per_message - 1) / per_message;
	job->messages = calloc(n_groups ? n_groups : 1, sizeof(*job->messages));
	if (!job->messages) {
		free(tiles);
		return false;
	}

	bool ok = true;
	for (int first = 0; first < n_tiles; first += per_message) {
		int count = n_tiles - first < per_message
			? n_tiles - first : per_message;

		RFX_MESSAGE* message = rfx_encode_message(self->rfx,
				&tiles[first], count, job->buffer->pixels,
				job->width, job->height, job->stride);
		if (!message) {
			ok = false;
			break;
		}

		wStream* s = Stream_New(NULL, (size_t)count * 4096 + 1024);
		bool written = s && rfx_write_message(self->rfx, s, message);
		rfx_message_free(self->rfx, message);
		if (!written) {
			Stream_Free(s, TRUE);
			ok = false;
			break;
		}
		job->messages[job->n_messages++] = s;
	}

	free(tiles);
	return ok;
}

/* Planar, one bitmap per damaged tile. */
static bool encode_planar(struct rdp_encoder* self, struct rdp_encode_job* job)
{
	RFX_RECT* tiles = NULL;
	int n_tiles = damaged_tiles(job, &tiles);
	if (n_tiles < 0)
		return false;

	job->bitmaps = calloc(n_tiles ? n_tiles : 1, sizeof(*job->bitmaps));
	if (!job->bitmaps) {
		free(tiles);
		return false;
	}

	bool ok = true;
	const uint8_t* pixels = job->buffer->pixels;
	for (int i = 0; i < n_tiles; ++i) {
		struct rdp_encoded_bitmap* b = &job->bitmaps[job->n_bitmaps];
		b->x = tiles[i].x;
		b->y = tiles[i].y;
		b->width = tiles[i].width;
		b->height = tiles[i].height;

		/* No reset per tile: the context was made for 64x64, the
		 * compressor takes the tile's size from its arguments, and a
		 * reset reallocates its four buffers each time (WinPR keeps a
		 * block only if it is larger than asked for), ~72 kB copied
		 * per tile. test/unit/planar-test.c pins that the output is the
		 * same. */
		const uint8_t* src = pixels + (size_t)b->y * job->stride
			+ (size_t)b->x * 4;
		uint32_t size = 0;
		b->data = freerdp_bitmap_compress_planar(self->planar, src,
				job->pixel_format, b->width, b->height,
				job->stride, NULL, &size);
		if (!b->data) {
			ok = false;
			break;
		}
		b->length = size;
		job->n_bitmaps++;
	}

	free(tiles);
	return ok;
}

/*
 * RemoteFX Progressive for the graphics pipeline: one message with every
 * damaged tile, at full quality in one go (FreeRDP's encoder has no
 * passes). It keeps no state between frames. The result lives in the
 * context and is copied out, so the next frame can't overwrite it before
 * it is sent.
 */
static bool encode_progressive(struct rdp_encoder* self,
		struct rdp_encode_job* job)
{
	RFX_RECT* tiles = NULL;
	int n_tiles = damaged_tiles(job, &tiles);
	if (n_tiles <= 0) {
		free(tiles);
		return n_tiles == 0;
	}

	REGION16 region;
	region16_init(&region);
	bool ok = true;
	for (int i = 0; i < n_tiles && ok; ++i) {
		RECTANGLE_16 r = {
			.left = tiles[i].x,
			.top = tiles[i].y,
			.right = tiles[i].x + tiles[i].width,
			.bottom = tiles[i].y + tiles[i].height,
		};
		ok = region16_union_rect(&region, &region, &r);
	}
	free(tiles);
	if (!ok) {
		region16_uninit(&region);
		return false;
	}
	job->bounds = *region16_extents(&region);

	BYTE* data = NULL;
	UINT32 size = 0;
	int rc = progressive_compress(self->progressive, job->buffer->pixels,
			job->stride * job->height, job->pixel_format, job->width,
			job->height, job->stride, &region, &data, &size);
	region16_uninit(&region);
	if (rc < 0)
		return false;
	if (rc == 0 || size == 0)
		return true;

	job->messages = calloc(1, sizeof(*job->messages));
	wStream* s = job->messages ? Stream_New(NULL, size) : NULL;
	if (!s)
		return false;
	Stream_Write(s, data, size);
	job->messages[0] = s;
	job->n_messages = 1;
	return true;
}

void rdp_encode(struct rdp_encoder* self, struct rdp_encode_job* job)
{
	uint64_t start = gettime_us();
	switch (job->codec) {
	case RDP_CODEC_REMOTEFX:
		job->ok = encode_remotefx(self, job);
		break;
	case RDP_CODEC_PLANAR:
		job->ok = encode_planar(self, job);
		break;
	case RDP_CODEC_GFX_PROGRESSIVE:
		job->ok = encode_progressive(self, job);
		break;
	}
	job->encode_us = gettime_us() - start;
}

void rdp_encode_job_free(struct rdp_encode_job* job)
{
	for (size_t i = 0; i < job->n_messages; ++i)
		Stream_Free(job->messages[i], TRUE);
	free(job->messages);
	for (size_t i = 0; i < job->n_bitmaps; ++i)
		free(job->bitmaps[i].data);
	free(job->bitmaps);
	free(job->rects);
	wv_buffer_release(job->buffer);
	free(job);
}
