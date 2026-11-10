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
 * Mouse and keyboard for test/mwc-rdp.sh: drives the compositor the RDP
 * client runs in. One command per line on stdin, "ok" once the compositor
 * has it:
 *
 *   move X Y          absolute position on the output
 *   button CODE 0|1   evdev button (272 is BTN_LEFT), released or pressed
 *   key CODE 0|1      evdev key code, released or pressed
 *   wheel N           N notches, positive is down
 *
 * Usage: vinput WIDTH HEIGHT
 *
 * Keys go by code, not by symbol: the client sends physical scancodes, and
 * what they mean is up to the layout on wayrdp's side.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"

static struct wl_seat* seat;
static struct zwp_virtual_keyboard_manager_v1* kb_manager;
static struct zwlr_virtual_pointer_manager_v1* ptr_manager;

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void global(void* data, struct wl_registry* registry, uint32_t name,
		const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface,
				zwp_virtual_keyboard_manager_v1_interface.name) == 0)
		kb_manager = wl_registry_bind(registry, name,
				&zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (strcmp(interface,
				zwlr_virtual_pointer_manager_v1_interface.name) == 0)
		ptr_manager = wl_registry_bind(registry, name,
				&zwlr_virtual_pointer_manager_v1_interface, 1);
}

static void global_remove(void* data, struct wl_registry* registry,
		uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = global,
	.global_remove = global_remove,
};

/* Any keymap does: the client only passes the codes on. */
static void send_keymap(struct zwp_virtual_keyboard_v1* keyboard)
{
	struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap* map = xkb_keymap_new_from_names(ctx, &names, 0);
	char* text = map ? xkb_keymap_get_as_string(map,
			XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	if (!text) {
		fprintf(stderr, "vinput: no keymap\n");
		exit(1);
	}
	size_t size = strlen(text) + 1;
	int fd = memfd_create("vinput-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
		perror("vinput");
		exit(1);
	}
	zwp_virtual_keyboard_v1_keymap(keyboard,
			WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(map);
	xkb_context_unref(ctx);
}

int main(int argc, char* argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage: vinput WIDTH HEIGHT\n");
		return 2;
	}
	int width = atoi(argv[1]), height = atoi(argv[2]);

	struct wl_display* display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "vinput: no compositor\n");
		return 1;
	}
	struct wl_registry* registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !kb_manager || !ptr_manager) {
		fprintf(stderr, "vinput: no virtual input protocols\n");
		return 1;
	}

	struct zwp_virtual_keyboard_v1* keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
				kb_manager, seat);
	struct zwlr_virtual_pointer_v1* pointer =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
				ptr_manager, seat);
	send_keymap(keyboard);
	wl_display_roundtrip(display);

	setvbuf(stdout, NULL, _IOLBF, 0);
	char line[256];
	while (fgets(line, sizeof(line), stdin)) {
		char cmd[16];
		int a = 0, b = 0;
		int n = sscanf(line, "%15s %d %d", cmd, &a, &b);
		const char* err = NULL;

		if (n == 3 && strcmp(cmd, "move") == 0) {
			/* The pixel's centre: X/width exactly lands just
			 * below X after the compositor's mapping. */
			zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
					2 * a + 1, 2 * b + 1, 2 * width,
					2 * height);
			zwlr_virtual_pointer_v1_frame(pointer);
		} else if (n == 3 && strcmp(cmd, "button") == 0) {
			zwlr_virtual_pointer_v1_button(pointer, now_ms(), a,
					b ? WL_POINTER_BUTTON_STATE_PRESSED
					  : WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(pointer);
		} else if (n == 3 && strcmp(cmd, "key") == 0) {
			zwp_virtual_keyboard_v1_key(keyboard, now_ms(), a,
					b ? WL_KEYBOARD_KEY_STATE_PRESSED
					  : WL_KEYBOARD_KEY_STATE_RELEASED);
		} else if (n == 2 && strcmp(cmd, "wheel") == 0) {
			zwlr_virtual_pointer_v1_axis_source(pointer,
					WL_POINTER_AXIS_SOURCE_WHEEL);
			zwlr_virtual_pointer_v1_axis_discrete(pointer, now_ms(),
					WL_POINTER_AXIS_VERTICAL_SCROLL,
					wl_fixed_from_int(15 * a), a);
			zwlr_virtual_pointer_v1_frame(pointer);
		} else {
			err = "bad command";
		}

		if (wl_display_roundtrip(display) < 0) {
			fprintf(stderr, "vinput: compositor gone\n");
			return 1;
		}
		puts(err ? err : "ok");
	}
	return 0;
}
