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
 * The RDP side of wayrdp, as seen from main.c: a listener, one active client,
 * and the frames that go to it. See docs/wayrdp-Entwurf.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wv_buffer;
struct pixman_region16;
struct rdp_server;

/* Input from the active client, already in Linux terms. */
struct rdp_input_handler {
	/* code: KEY_* from linux/input-event-codes.h */
	void (*key)(void* userdata, uint32_t code, bool pressed);
	/* From the Unicode keyboard events (on-screen keyboard, IME). */
	void (*keysym)(void* userdata, uint32_t keysym, bool pressed);
	/* Releases every key the client pressed, e.g. after Alt-Tab on the
	 * client's side, or when the client goes. */
	void (*release_all)(void* userdata);
	/* RDP_SYNC_* from rdp/keymap.h: the lock keys as the client has them. */
	void (*sync_locks)(void* userdata, uint32_t locks);
	/* x, y from 0 to 1 over the desktop. */
	void (*motion)(void* userdata, double x, double y);
	/* code: BTN_LEFT, … */
	void (*button)(void* userdata, uint32_t code, bool pressed);
	/* value120: 120 is one notch; positive is down or right. */
	void (*axis)(void* userdata, bool horizontal, int32_t value120);
	/* Ends a group of pointer events. */
	void (*frame)(void* userdata);
};

struct rdp_server_config {
	const char* address;
	uint16_t port;
	const char* certificate_file;
	const char* private_key_file;
	const char* sam_file;

	/* The client came, went, minimised or restored: the capture should
	 * start or stop, see rdp_server_wants_frames(). */
	void (*on_wants_frames_changed)(void* userdata);
	const struct rdp_input_handler* input;
	void* userdata;
};

/* Opens the listener. NULL on failure, with the reason logged. */
struct rdp_server* rdp_server_new(const struct rdp_server_config* config);
void rdp_server_destroy(struct rdp_server* self);

/* Whether a client wants frames, so the capture should run. */
bool rdp_server_wants_frames(const struct rdp_server* self);

/* The size of the captured output, for clients that connect before the
 * first frame. */
void rdp_server_set_desktop_size(struct rdp_server* self, uint32_t width,
		uint32_t height);

/* A captured frame and its damage in buffer coordinates. The server keeps a
 * reference on the newest buffer and encodes from it. */
void rdp_frame_submit(struct rdp_server* self, struct wv_buffer* buffer,
		const struct pixman_region16* damage);

/* Text copied in the session, for the client's clipboard (M3). */
void rdp_server_set_selection_text(struct rdp_server* self, const char* text,
		size_t len);

struct rdp_server_stats {
	uint32_t frames;
	uint64_t damage_area;
	uint32_t frames_sent;
};
void rdp_server_get_stats(const struct rdp_server* self,
		struct rdp_server_stats* stats);

const char* rdp_freerdp_version(void);
