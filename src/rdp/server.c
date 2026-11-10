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
#include <inttypes.h>
#include <pixman.h>
#include <freerdp/freerdp.h>

#include "rdp/server.h"
#include "buffer.h"
#include "log.h"

struct rdp_server {
	struct rdp_server_config config;
	struct rdp_server_stats stats;
};

struct rdp_server* rdp_server_new(const struct rdp_server_config* config)
{
	struct rdp_server* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->config = *config;

	wv_log(WV_LOG_INFO, "FreeRDP %s", rdp_freerdp_version());
	wv_log(WV_LOG_WARNING, "No RDP listener yet (M1). %s:%u is not opened",
			config->address, config->port);
	return self;
}

void rdp_server_destroy(struct rdp_server* self)
{
	free(self);
}

bool rdp_server_wants_frames(const struct rdp_server* self)
{
	/* No peers before M1. */
	return false;
}

static uint64_t region_area(const struct pixman_region16* region)
{
	int n = 0;
	const pixman_box16_t* box = pixman_region_rectangles(
			(pixman_region16_t*)region, &n);
	uint64_t area = 0;
	for (int i = 0; i < n; ++i)
		area += (uint64_t)(box[i].x2 - box[i].x1) * (box[i].y2 - box[i].y1);
	return area;
}

void rdp_frame_submit(struct rdp_server* self, struct wv_buffer* buffer,
		const struct pixman_region16* damage)
{
	/* M1 holds a reference here and encodes in a worker. */
	self->stats.frames++;
	self->stats.damage_area += region_area(damage);
	wv_trace("frame %ux%u, damage %"PRIu64" px", buffer->width,
			buffer->height, region_area(damage));
}

void rdp_server_set_selection_text(struct rdp_server* self, const char* text,
		size_t len)
{
	wv_log(WV_LOG_DEBUG, "Selection changed (%zu bytes), no client yet", len);
}

void rdp_server_get_stats(const struct rdp_server* self,
		struct rdp_server_stats* stats)
{
	*stats = self->stats;
}

const char* rdp_freerdp_version(void)
{
	return freerdp_get_version_string();
}
