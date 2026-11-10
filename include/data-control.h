/*
 * Copyright (c) 2020 Scott Moreau
 * Copyright (c) 2020 - 2026 Andri Yngvason
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
 * The session's clipboard through ext-data-control-v1 (or the older
 * wlr-data-control-unstable-v1, same shape).
 *
 * Nothing is copied ahead: the current offer is kept and read only when
 * asked (data_control_read), and wayrdp's own selection carries no data,
 * a paste hands the fd on (on_paste). The primary selection stays in the
 * session, RDP has none.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sys/queue.h"

struct wl_seat;
struct zwlr_data_control_manager_v1;
struct ext_data_control_manager_v1;
struct dc_offer;
struct dc_read;

LIST_HEAD(dc_read_list, dc_read);

struct data_control_callbacks {
	/* The session's clipboard changed: it has text now, or not. Not for
	 * wayrdp's own selection. */
	void (*on_selection_changed)(void* userdata, bool has_text);
	/* A program pastes wayrdp's own selection: write UTF-8 text to fd
	 * and close it. */
	void (*on_paste)(void* userdata, int fd);
};

struct data_control {
	/* One of the two, set before data_control_init(). */
	struct zwlr_data_control_manager_v1* wlr_manager;
	struct ext_data_control_manager_v1* ext_manager;

	const struct data_control_callbacks* callbacks;
	void* userdata;

	void* device;
	/* The offer being announced, and the session's current one with
	 * text (NULL: none, or it is wayrdp's own). */
	struct dc_offer* pending;
	struct dc_offer* current;
	bool has_text;

	void* own_source;
	/* x-wayrdp-client-<8 hex digits>, marks wayrdp's own selection. */
	char own_mime[32];

	struct dc_read_list reads;
};

void data_control_init(struct data_control* self, struct wl_seat* seat,
		const struct data_control_callbacks* callbacks, void* userdata);
void data_control_destroy(struct data_control* self);

/* Reads the session's clipboard as UTF-8. done() gets the text (not
 * NUL-terminated) or NULL, later, from the main loop; never from inside
 * this call. Returns -1 (done() not called) if there is nothing to read. */
int data_control_read(struct data_control* self,
		void (*done)(void* userdata, const char* text, size_t len),
		void* userdata);

/* Makes wayrdp's own selection (text from the client) the session's. */
void data_control_offer(struct data_control* self);
/* Takes it back, if it still is the session's. */
void data_control_withdraw(struct data_control* self);
