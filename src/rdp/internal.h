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
#include <freerdp/codec/progressive.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/disp.h>
#include <pthread.h>

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
	/* A password was checked (NTLM against the SAM file, or PAM): only
	 * then a connection that doesn't log on counts as a failed login.
	 * mstsc drops connections before that (certificate prompt, its own
	 * login dialog), and those must not lock it out. */
	bool password_checked;
	/* The connection sequence is through (PostConnect). */
	bool connected;
	bool activated;
	/* -a pam: the password is being checked; Activate waits for it. */
	bool pam_pending;
	bool activate_waiting;
	/* Closes the connection if it doesn't log on in time. */
	struct aml_timer* login_timer;
	bool suppressed;
	bool closing;

	/* Flow control: frames carry ids the client acknowledges (frame
	 * markers of the old way, or the graphics pipeline's own). Some
	 * clients never acknowledge; until one ack has come, a timeout
	 * switches the waiting off. */
	bool frame_acks;
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

	/* Clipboard (clipboard.c) */
	CliprdrServerContext* cliprdr;
	struct aml_handler* cliprdr_handler;
	bool clip_ready;
	bool client_has_text;
	/* Pastes waiting for the client's answer. */
	int paste_fds[8];
	int n_paste_fds;
	bool paste_requested;
	struct aml_timer* paste_timer;

	/* Display control (display.c): the layout comes from FreeRDP's
	 * channel thread, through an eventfd, and settles for a moment
	 * before the output follows. */
	DispServerContext* disp;
	bool disp_tried;
	bool disp_lock_init;
	int disp_eventfd;
	struct aml_handler* disp_handler;
	struct aml_timer* disp_timer;
	pthread_mutex_t disp_lock;
	uint32_t disp_width, disp_height;
	/* The caps go once the client created the channel. */
	uint32_t disp_channel_id;
	bool disp_caps_sent;

	/* Graphics pipeline (gfx.c). */
	RdpgfxServerContext* gfx;
	struct aml_handler* gfx_handler;
	struct aml_timer* gfx_timer;
	bool gfx_tried;
	/* The client can't, or the pipeline broke off: the old way. */
	bool gfx_failed;
	/* Capabilities confirmed. */
	bool gfx_ready;
	bool gfx_dispatching;
	uint32_t gfx_version;
	bool gfx_surface;
	uint16_t gfx_surface_id;

	/* Pointer images the client has cached (cursor.c). */
	struct {
		uint32_t hash;
		uint64_t used;
	} cursor_cache[32];
	uint32_t cursor_cache_size;
	uint64_t cursor_tick;
	uint32_t cursor_hash_shown;
	bool cursor_hidden_shown;
	/* A picture went out since the activation: only then the pointer
	 * (wlfreerdp3 sometimes never draws if the pointer comes first). */
	bool picture_sent;
};

LIST_HEAD(rdp_peer_list, rdp_peer);

/* Writes a paste to a program, maybe in several goes. */
struct rdp_fd_writer;
LIST_HEAD(rdp_fd_writer_list, rdp_fd_writer);

struct rdp_peer_context {
	rdpContext base;
	struct rdp_peer* peer;
	HANDLE vcm;
};

enum rdp_codec {
	RDP_CODEC_REMOTEFX,
	RDP_CODEC_PLANAR,
	/* RemoteFX Progressive over the graphics pipeline. */
	RDP_CODEC_GFX_PROGRESSIVE,
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
	/* What the damage covers (GFX). */
	RECTANGLE_16 bounds;
	wStream** messages;
	size_t n_messages;
	struct rdp_encoded_bitmap* bitmaps;
	size_t n_bitmaps;
	uint64_t encode_us;
};

struct rdp_encoder {
	RFX_CONTEXT* rfx;
	BITMAP_PLANAR_CONTEXT* planar;
	PROGRESSIVE_CONTEXT* progressive;
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
	/* An output size asked for, and until when to wait for frames of
	 * it before the client is resized to what there is. */
	uint32_t want_width, want_height;
	uint64_t want_until_us;
	struct aml_timer* want_timer;

	/* The pointer as it is in the session. */
	uint8_t* cursor_pixels;
	uint32_t cursor_width, cursor_height, cursor_hot_x, cursor_hot_y;
	uint32_t cursor_hash;
	bool cursor_hidden;

	/* Whether the session's clipboard has text, for the next client. */
	bool session_has_text;
	struct rdp_fd_writer_list writers;
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
	/* PAM checks running in the worker. */
	int pam_jobs;
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
/* Starts the output again after the way of sending changed. */
void rdp_peer_reactivated(struct rdp_peer* self);

/* input.c */
void rdp_input_register(struct rdp_peer* self);
/* Lets go of the keys and buttons this client holds. */
void rdp_input_release(struct rdp_peer* self);

/* clipboard.c */
/* Opens the channel once the client has joined it (from Activate). */
void rdp_clipboard_open(struct rdp_peer* self);
void rdp_clipboard_close(struct rdp_peer* self);
/* The session's clipboard changed: tell the client. */
void rdp_clipboard_announce(struct rdp_peer* self);
void rdp_clipboard_paste(struct rdp_peer* self, int fd);
void rdp_clipboard_writers_destroy(struct rdp_server* server);

/* display.c */
/* Opens the display control channel once drdynvc is ready. */
void rdp_display_check(struct rdp_peer* self);
void rdp_display_close(struct rdp_peer* self);

/* gfx.c */
/* Whether wayrdp offers the pipeline at all (not in the tests of the old
 * way). */
bool rdp_gfx_offered(void);
/* The client announced the pipeline, and it isn't up yet. */
bool rdp_gfx_pending(const struct rdp_peer* self);
/* Opens the channel once drdynvc is ready. */
void rdp_gfx_check(struct rdp_peer* self);
/* How long a client may take until its capabilities come. */
void rdp_gfx_arm_timer(struct rdp_peer* self);
void rdp_gfx_close(struct rdp_peer* self);
bool rdp_gfx_reset_surface(struct rdp_peer* self, uint32_t width,
		uint32_t height);
bool rdp_gfx_send(struct rdp_peer* self, const struct rdp_encode_job* job);

/* cursor.c */
void rdp_cursor_reset(struct rdp_peer* self);
/* Shows the session's pointer on the client, if it changed. */
void rdp_cursor_send(struct rdp_peer* self);

/* server.c, for the peer and display control */
/* Asks for an output size (clamped first); false if that can't be. */
bool rdp_server_request_size(struct rdp_server* self, uint32_t width,
		uint32_t height, uint32_t* out_width, uint32_t* out_height);

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
