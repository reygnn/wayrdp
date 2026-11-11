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
 * The pointer as the client draws it: the session's pointer image goes out
 * as a 32-bit color pointer (with alpha), and the client draws it itself at
 * its own mouse position, so it doesn't lag behind by a round trip.
 *
 * Images the client has seen are in its pointer cache; showing one again
 * is a PointerCached with its index.
 */

#include <stdlib.h>
#include <string.h>
#include <freerdp/freerdp.h>
#include <freerdp/pointer.h>

#include "rdp/internal.h"
#include "rdp/convert.h"
#include "log.h"

#define MAX_CACHE 32
/* Without LARGE_POINTER_FLAG_* the client takes at most 96x96. */
#define SMALL_MAX 96
#define LARGE_MAX 384

static uint32_t fnv1a(uint32_t h, const void* data, size_t len)
{
	const uint8_t* p = data;
	for (size_t i = 0; i < len; ++i) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

void rdp_server_set_cursor(struct rdp_server* self, const uint8_t* pixels,
		uint32_t width, uint32_t height, uint32_t stride,
		uint32_t hot_x, uint32_t hot_y)
{
	if (width == 0 || height == 0 || width > LARGE_MAX
			|| height > LARGE_MAX) {
		wv_log(WV_LOG_DEBUG, "Pointer image %ux%u ignored", width, height);
		return;
	}

	uint8_t* copy = malloc((size_t)width * height * 4);
	if (!copy)
		return;
	uint32_t hash = 2166136261u;
	for (uint32_t y = 0; y < height; ++y) {
		memcpy(copy + (size_t)y * width * 4, pixels + (size_t)y * stride,
				(size_t)width * 4);
	}
	hash = fnv1a(hash, copy, (size_t)width * height * 4);
	uint32_t meta[4] = { width, height, hot_x, hot_y };
	hash = fnv1a(hash, meta, sizeof(meta));

	free(self->cursor_pixels);
	self->cursor_pixels = copy;
	self->cursor_width = width;
	self->cursor_height = height;
	self->cursor_hot_x = hot_x;
	self->cursor_hot_y = hot_y;
	self->cursor_hash = hash;
	self->cursor_hidden = false;

	if (self->active)
		rdp_cursor_send(self->active);
}

void rdp_server_set_cursor_visible(struct rdp_server* self, bool visible)
{
	self->cursor_hidden = !visible;
	if (self->active)
		rdp_cursor_send(self->active);
}

void rdp_cursor_reset(struct rdp_peer* self)
{
	rdpSettings* settings = self->client->context->settings;
	uint32_t size = freerdp_settings_get_uint32(settings,
			FreeRDP_PointerCacheSize);
	self->cursor_cache_size = size > MAX_CACHE ? MAX_CACHE : size;
	memset(self->cursor_cache, 0, sizeof(self->cursor_cache));
	self->cursor_tick = 0;
	self->cursor_hash_shown = 0;
	self->cursor_hidden_shown = false;
	self->picture_sent = false;
}

/* The cache slot to use: the one with the image, or the least recently
 * used one. *hit tells which. */
static uint32_t cache_slot(struct rdp_peer* self, uint32_t hash, bool* hit)
{
	uint32_t lru = 0;
	for (uint32_t i = 0; i < self->cursor_cache_size; ++i) {
		if (self->cursor_cache[i].used && self->cursor_cache[i].hash == hash) {
			*hit = true;
			return i;
		}
		if (self->cursor_cache[i].used < self->cursor_cache[lru].used)
			lru = i;
	}
	*hit = false;
	return lru;
}

static bool send_image(struct rdp_peer* self, uint32_t slot)
{
	struct rdp_server* server = self->server;
	rdpContext* context = self->client->context;
	rdpPointerUpdate* pointer = context->update->pointer;

	uint32_t large = freerdp_settings_get_uint32(context->settings,
			FreeRDP_LargePointerFlag);
	uint32_t max = (large & LARGE_POINTER_FLAG_384x384) ? LARGE_MAX
		: (large & LARGE_POINTER_FLAG_96x96) ? SMALL_MAX : SMALL_MAX;

	/* Larger than the client takes: the part from the top left. */
	uint32_t w = server->cursor_width < max ? server->cursor_width : max;
	uint32_t h = server->cursor_height < max ? server->cursor_height : max;
	uint32_t hot_x = server->cursor_hot_x < w ? server->cursor_hot_x : w - 1;
	uint32_t hot_y = server->cursor_hot_y < h ? server->cursor_hot_y : h - 1;

	size_t xor_len = (size_t)w * h * 4;
	size_t and_len = rdp_pointer_and_size(w, h);
	uint8_t* xor_mask = malloc(xor_len);
	uint8_t* and_mask = malloc(and_len);
	if (!xor_mask || !and_mask) {
		free(xor_mask);
		free(and_mask);
		return false;
	}
	rdp_pointer_convert(server->cursor_pixels, w, h,
			server->cursor_width * 4, xor_mask, and_mask);

	BOOL ok;
	if (w > SMALL_MAX || h > SMALL_MAX) {
		POINTER_LARGE_UPDATE update = {
			.xorBpp = 32,
			.cacheIndex = slot,
			.hotSpotX = hot_x,
			.hotSpotY = hot_y,
			.width = w,
			.height = h,
			.lengthAndMask = and_len,
			.lengthXorMask = xor_len,
			.xorMaskData = xor_mask,
			.andMaskData = and_mask,
		};
		ok = pointer->PointerLarge(context, &update);
	} else {
		POINTER_NEW_UPDATE update = {
			.xorBpp = 32,
			.colorPtrAttr = {
				.cacheIndex = slot,
				.hotSpotX = hot_x,
				.hotSpotY = hot_y,
				.width = w,
				.height = h,
				.lengthAndMask = and_len,
				.lengthXorMask = xor_len,
				.xorMaskData = xor_mask,
				.andMaskData = and_mask,
			},
		};
		ok = pointer->PointerNew(context, &update);
	}
	free(xor_mask);
	free(and_mask);
	wv_log(WV_LOG_DEBUG, "Pointer %ux%u, hotspot %u,%u, new in slot %u",
			w, h, hot_x, hot_y, slot);
	return ok;
}

void rdp_cursor_send(struct rdp_peer* self)
{
	struct rdp_server* server = self->server;
	if (!self->activated || self->closing || !self->picture_sent
			|| !server->cursor_pixels)
		return;

	rdpContext* context = self->client->context;
	rdpPointerUpdate* pointer = context->update->pointer;

	if (server->cursor_hidden) {
		if (self->cursor_hidden_shown)
			return;
		POINTER_SYSTEM_UPDATE update = { .type = SYSPTR_NULL };
		pointer->PointerSystem(context, &update);
		self->cursor_hidden_shown = true;
		self->cursor_hash_shown = 0;
		wv_log(WV_LOG_DEBUG, "Pointer hidden");
		return;
	}

	if (!self->cursor_hidden_shown
			&& self->cursor_hash_shown == server->cursor_hash)
		return;

	bool hit = false;
	uint32_t slot = 0;
	if (self->cursor_cache_size > 0)
		slot = cache_slot(self, server->cursor_hash, &hit);

	BOOL ok;
	if (hit) {
		POINTER_CACHED_UPDATE update = { .cacheIndex = slot };
		ok = pointer->PointerCached(context, &update);
		wv_log(WV_LOG_DEBUG, "Pointer from cache slot %u", slot);
	} else {
		ok = send_image(self, slot);
		if (ok && self->cursor_cache_size > 0)
			self->cursor_cache[slot].hash = server->cursor_hash;
	}
	if (!ok) {
		wv_log(WV_LOG_WARNING, "Sending the pointer failed");
		return;
	}

	if (self->cursor_cache_size > 0)
		self->cursor_cache[slot].used = ++self->cursor_tick;
	self->cursor_hash_shown = server->cursor_hash;
	self->cursor_hidden_shown = false;
	rdp_peer_write_blocked(self);
}
