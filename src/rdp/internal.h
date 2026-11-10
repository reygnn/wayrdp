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
 * What the files in src/rdp/ share. main.c only sees rdp/server.h.
 *
 * Threads: everything runs on aml's main loop except the encoding, which runs
 * as aml_work in a worker. The worker touches only the encoder's codec
 * contexts and the job; it never calls into the peer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/planar.h>

#include "sys/queue.h"
#include "rdp/server.h"
#include "rdp/auth.h"
#include "rdp/keymap.h"

struct aml_handler;
struct aml_idle;
struct aml_timer;
struct aml_work;
struct wv_buffer;

#define RDP_MAX_PEER_HANDLES 8
/* Frames sent but not acknowledged before the capture waits. */
#define RDP_MAX_FRAMES_IN_FLIGHT 2

struct rdp_peer {
	LIST_ENTRY(rdp_peer) link;
	struct rdp_server* server;
	freerdp_peer* client;
	uint64_t serial;

	struct aml_handler* handlers[RDP_MAX_PEER_HANDLES];
	int n_handlers;
	struct aml_handler* vcm_handler;
	struct aml_idle* close_idle;

	bool logged_on;
	bool activated;
	bool suppressed;
	bool closing;

	/* Flow control. Some clients never acknowledge; until one ack has
	 * come, a timeout switches the waiting off. */
	bool frame_markers;
	bool ack_seen;
	bool ack_wait;
	uint32_t frame_id;
	uint32_t last_ack;

	uint32_t frames_sent;
	uint64_t bytes_sent;
	uint64_t connected_us;

	struct rdp_key_decoder key_decoder;
	struct rdp_utf16_decoder utf16_decoder;
	/* Buttons this client holds down, to let go when it goes. */
	uint32_t buttons;
};

LIST_HEAD(rdp_peer_list, rdp_peer);

struct rdp_peer_context {
	rdpContext base;
	struct rdp_peer* peer;
	HANDLE vcm;
};

enum rdp_codec {
	RDP_CODEC_REMOTEFX,
	RDP_CODEC_PLANAR,
};

struct rdp_encoded_bitmap {
	uint16_t x, y, width, height;
	uint8_t* data;
	uint32_t length;
};

struct rdp_encode_job {
	struct rdp_server* server;
	uint64_t peer_serial;

	/* In */
	enum rdp_codec codec;
	struct wv_buffer* buffer;
	uint32_t width, height, stride;
	uint32_t pixel_format;
	uint32_t rlgr_mode;
	bool reset;
	size_t max_request_size;
	RFX_RECT* rects;
	int n_rects;

	/* Out */
	bool ok;
	wStream** messages;
	size_t n_messages;
	struct rdp_encoded_bitmap* bitmaps;
	size_t n_bitmaps;
	uint64_t encode_us;
};

struct rdp_encoder {
	RFX_CONTEXT* rfx;
	BITMAP_PLANAR_CONTEXT* planar;
	uint32_t width, height;
};

struct rdp_server {
	struct rdp_server_config config;
	struct rdp_server_stats stats;

	freerdp_listener* listener;
	struct aml_handler* listen_handlers[8];
	int n_listen_handlers;

	struct rdp_auth auth;
	struct rdp_peer_list peers;
	struct rdp_peer* active;
	uint64_t next_serial;

	/* The newest frame and what the client hasn't got of it yet. */
	struct wv_buffer* current;
	struct pixman_region16 pending;
	bool need_full;
	uint32_t width, height;
	uint32_t pixel_format;

	struct rdp_encoder encoder;
	bool encoding;
	bool encoder_reset;
	struct aml_timer* ack_timer;
	struct aml_ticker* stats_ticker;
	uint64_t encode_us_sum;
	uint32_t encode_count;
	uint64_t bytes_window;
	uint32_t frames_window;
};

/* listener.c */
int rdp_listener_open(struct rdp_server* self);
void rdp_listener_close(struct rdp_server* self);

/* peer.c */
struct rdp_peer* rdp_peer_new(struct rdp_server* server, freerdp_peer* client);
void rdp_peer_close(struct rdp_peer* self);
void rdp_peer_destroy(struct rdp_peer* self);
bool rdp_peer_wants_frames(const struct rdp_peer* self);
uint32_t rdp_peer_frames_in_flight(const struct rdp_peer* self);
void rdp_peer_send(struct rdp_peer* self, const struct rdp_encode_job* job);
bool rdp_peer_resize(struct rdp_peer* self, uint32_t width, uint32_t height);
bool rdp_peer_size_matches(const struct rdp_peer* self, uint32_t width,
		uint32_t height);
enum rdp_codec rdp_peer_codec(const struct rdp_peer* self);

/* input.c */
void rdp_input_register(struct rdp_peer* self);
/* Lets go of the keys and buttons this client holds. */
void rdp_input_release(struct rdp_peer* self);

/* encoder.c */
int rdp_encoder_init(struct rdp_encoder* self);
void rdp_encoder_fini(struct rdp_encoder* self);
void rdp_encode(struct rdp_encoder* self, struct rdp_encode_job* job);
void rdp_encode_job_free(struct rdp_encode_job* job);

/* server.c, called by the peer */
void rdp_server_peer_logged_on(struct rdp_server* self, struct rdp_peer* peer);
void rdp_server_peer_activated(struct rdp_server* self, struct rdp_peer* peer);
void rdp_server_peer_gone(struct rdp_server* self, struct rdp_peer* peer);
void rdp_server_refresh(struct rdp_server* self, const RECTANGLE_16* rects,
		int n_rects);
void rdp_server_output_changed(struct rdp_server* self);
void rdp_server_schedule(struct rdp_server* self);
