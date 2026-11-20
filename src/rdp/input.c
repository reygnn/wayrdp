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
 * The peer's input callbacks. Events count only from the active, activated
 * client; they go to main.c through rdp_input_handler.
 */

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>

#include "rdp/internal.h"
#include "log.h"

#define BUTTON_LEFT (1 << 0)
#define BUTTON_RIGHT (1 << 1)
#define BUTTON_MIDDLE (1 << 2)
#define BUTTON_SIDE (1 << 3)
#define BUTTON_EXTRA (1 << 4)

static const struct {
	uint32_t bit;
	uint32_t code;
} buttons[] = {
	{ BUTTON_LEFT, BTN_LEFT },
	{ BUTTON_RIGHT, BTN_RIGHT },
	{ BUTTON_MIDDLE, BTN_MIDDLE },
	{ BUTTON_SIDE, BTN_SIDE },
	{ BUTTON_EXTRA, BTN_EXTRA },
};

static struct rdp_peer* peer_from_input(rdpInput* input)
{
	return ((struct rdp_peer_context*)input->context)->peer;
}

/* The handler, if this peer may give input now. */
static const struct rdp_input_handler* handler_for(struct rdp_peer* self)
{
	struct rdp_server* server = self->server;
	/* Before the main loop has the connection: nothing of the server. */
	if (atomic_load(&self->on_thread))
		return NULL;
	if (server->active != self || !self->activated || self->closing)
		return NULL;
	return server->config.input;
}

static void* userdata(const struct rdp_peer* self)
{
	return self->server->config.userdata;
}

static BOOL on_synchronize(rdpInput* input, UINT32 flags)
{
	struct rdp_peer* self = peer_from_input(input);
	const struct rdp_input_handler* h = handler_for(self);
	if (!h)
		return TRUE;

	wv_log(WV_LOG_DEBUG, "Synchronize: locks 0x%x", flags);
	/* Comes when the client window gets the focus again: keys let go
	 * meanwhile (Alt-Tab) never sent their release. */
	h->release_all(userdata(self));
	h->sync_locks(userdata(self), flags);
	return TRUE;
}

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	struct rdp_peer* self = peer_from_input(input);
	const struct rdp_input_handler* h = handler_for(self);
	if (!h)
		return TRUE;

	uint32_t evdev = 0;
	bool pressed = false;
	if (!rdp_key_decode(&self->key_decoder, flags, code, &evdev, &pressed)) {
		if (!(flags & RDP_KBD_EXTENDED1))
			wv_log(WV_LOG_DEBUG, "Unknown scancode 0x%02x%s", code,
					(flags & RDP_KBD_EXTENDED) ? " (E0)" : "");
		return TRUE;
	}

	wv_trace("Key 0x%02x%s -> %u %s", code,
			(flags & RDP_KBD_EXTENDED) ? " (E0)" : "", evdev,
			pressed ? "down" : "up");
	h->key(userdata(self), evdev, pressed);
	return TRUE;
}

static BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code)
{
	struct rdp_peer* self = peer_from_input(input);
	const struct rdp_input_handler* h = handler_for(self);
	if (!h)
		return TRUE;

	uint32_t cp = rdp_unicode_key(&self->unicode_keys, code,
			flags & RDP_KBD_RELEASE);
	if (!cp)
		return TRUE;

	xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
	if (sym == XKB_KEY_NoSymbol) {
		wv_log(WV_LOG_DEBUG, "No keysym for U+%04X", cp);
		return TRUE;
	}
	h->keysym(userdata(self), sym, !(flags & RDP_KBD_RELEASE));
	return TRUE;
}

static void move(struct rdp_peer* self, const struct rdp_input_handler* h,
		UINT16 x, UINT16 y)
{
	struct rdp_server* server = self->server;
	if (!server->width || !server->height)
		return;
	wv_trace("Motion %u,%u", x, y);
	h->motion(userdata(self), (double)x / server->width,
			(double)y / server->height);
}

static void set_button(struct rdp_peer* self,
		const struct rdp_input_handler* h, uint32_t bit, bool pressed)
{
	if (!!(self->buttons & bit) == pressed)
		return;
	if (pressed)
		self->buttons |= bit;
	else
		self->buttons &= ~bit;

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
		if (buttons[i].bit == bit) {
			wv_log(WV_LOG_DEBUG, "Button 0x%x %s", buttons[i].code,
					pressed ? "down" : "up");
			h->button(userdata(self), buttons[i].code, pressed);
		}
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	struct rdp_peer* self = peer_from_input(input);
	const struct rdp_input_handler* h = handler_for(self);
	if (!h)
		return TRUE;

	if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
		int32_t rotation = rdp_wheel_rotation(flags);
		wv_log(WV_LOG_DEBUG, "Wheel %s %d", (flags & PTR_FLAGS_WHEEL)
				? "vertical" : "horizontal", rotation);
		/* Up in RDP is negative in Wayland; right is right in both. */
		if (flags & PTR_FLAGS_WHEEL)
			h->axis(userdata(self), false, -rotation);
		else
			h->axis(userdata(self), true, rotation);
		h->frame(userdata(self));
		return TRUE;
	}

	if (flags & PTR_FLAGS_MOVE)
		move(self, h, x, y);

	bool down = flags & PTR_FLAGS_DOWN;
	if (flags & PTR_FLAGS_BUTTON1) {
		move(self, h, x, y);
		set_button(self, h, BUTTON_LEFT, down);
	}
	if (flags & PTR_FLAGS_BUTTON2) {
		move(self, h, x, y);
		set_button(self, h, BUTTON_RIGHT, down);
	}
	if (flags & PTR_FLAGS_BUTTON3) {
		move(self, h, x, y);
		set_button(self, h, BUTTON_MIDDLE, down);
	}

	h->frame(userdata(self));
	return TRUE;
}

static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x,
		UINT16 y)
{
	struct rdp_peer* self = peer_from_input(input);
	const struct rdp_input_handler* h = handler_for(self);
	if (!h)
		return TRUE;

	bool down = flags & PTR_XFLAGS_DOWN;
	move(self, h, x, y);
	if (flags & PTR_XFLAGS_BUTTON1)
		set_button(self, h, BUTTON_SIDE, down);
	if (flags & PTR_XFLAGS_BUTTON2)
		set_button(self, h, BUTTON_EXTRA, down);
	h->frame(userdata(self));
	return TRUE;
}

void rdp_input_register(struct rdp_peer* self)
{
	rdpInput* input = self->client->context->input;
	input->SynchronizeEvent = on_synchronize;
	input->KeyboardEvent = on_keyboard;
	input->UnicodeKeyboardEvent = on_unicode_keyboard;
	input->MouseEvent = on_mouse;
	input->ExtendedMouseEvent = on_extended_mouse;
}

void rdp_input_release(struct rdp_peer* self)
{
	const struct rdp_input_handler* h = self->server->config.input;
	if (!h)
		return;

	if (self->buttons) {
		for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
			if (self->buttons & buttons[i].bit)
				h->button(userdata(self), buttons[i].code, false);
		self->buttons = 0;
		h->frame(userdata(self));
	}
	h->release_all(userdata(self));
	self->key_decoder.pause_pending = false;
	self->unicode_keys = (struct rdp_unicode_keys){ 0 };
}
