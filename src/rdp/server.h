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
 * The RDP side of wayrdp, as seen from main.c. M0 has only the frame path and
 * the FreeRDP version; the listener, the peer and the channels follow in M1
 * (see docs/wayrdp-Entwurf.md).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wv_buffer;
struct pixman_region16;
struct rdp_server;

struct rdp_server_config {
	const char* address;
	uint16_t port;
};

struct rdp_server* rdp_server_new(const struct rdp_server_config* config);
void rdp_server_destroy(struct rdp_server* self);

/* Whether a client wants frames, so the capture should run. */
bool rdp_server_wants_frames(const struct rdp_server* self);

/* A captured frame and its damage in buffer coordinates. The server takes a
 * reference on the buffer while it encodes it, and drops it after. */
void rdp_frame_submit(struct rdp_server* self, struct wv_buffer* buffer,
		const struct pixman_region16* damage);

/* Text copied in the session, for the client's clipboard. */
void rdp_server_set_selection_text(struct rdp_server* self, const char* text,
		size_t len);

struct rdp_server_stats {
	uint32_t frames;
	uint64_t damage_area;
};
void rdp_server_get_stats(const struct rdp_server* self,
		struct rdp_server_stats* stats);

const char* rdp_freerdp_version(void);
