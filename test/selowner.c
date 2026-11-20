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
 * A selection owner for test/mwc-rdp.sh that writes as a plain program
 * does: one write() of everything, trusting the pipe to block. Reads its
 * text from stdin, takes the selection through ext-data-control, prints
 * "ok" once it holds it and answers every paste until killed. A write that
 * fails (EAGAIN on a pipe someone made non-blocking) goes to stderr.
 *
 * Usage: selowner < FILE
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1.h"

static struct wl_seat* seat;
static struct ext_data_control_manager_v1* manager;
static char* text;
static size_t text_len;

static void global(void* data, struct wl_registry* registry, uint32_t name,
		const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_seat_interface.name) == 0 && !seat)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(registry, name,
				&ext_data_control_manager_v1_interface, 1);
}

static void global_remove(void* data, struct wl_registry* registry,
		uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = global,
	.global_remove = global_remove,
};

static void on_send(void* data, struct ext_data_control_source_v1* source,
		const char* mime, int32_t fd)
{
	size_t off = 0;
	while (off < text_len) {
		ssize_t n = write(fd, text + off, text_len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "selowner: write after %zu of %zu bytes: %s\n",
					off, text_len, strerror(errno));
			break;
		}
		off += n;
	}
	close(fd);
}

static void on_cancelled(void* data, struct ext_data_control_source_v1* source)
{
	exit(0);
}

static const struct ext_data_control_source_v1_listener source_listener = {
	.send = on_send,
	.cancelled = on_cancelled,
};

int main(void)
{
	size_t cap = 1 << 16;
	text = malloc(cap);
	for (;;) {
		if (text_len == cap)
			text = realloc(text, cap *= 2);
		ssize_t n = read(0, text + text_len, cap - text_len);
		if (n <= 0)
			break;
		text_len += n;
	}

	struct wl_display* display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "selowner: no Wayland display\n");
		return 1;
	}
	struct wl_registry* registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !manager) {
		fprintf(stderr, "selowner: no seat or ext-data-control\n");
		return 1;
	}

	struct ext_data_control_device_v1* device =
		ext_data_control_manager_v1_get_data_device(manager, seat);
	struct ext_data_control_source_v1* source =
		ext_data_control_manager_v1_create_data_source(manager);
	ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
	ext_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
	ext_data_control_device_v1_set_selection(device, source);
	wl_display_roundtrip(display);
	printf("ok\n");
	fflush(stdout);

	while (wl_display_dispatch(display) >= 0)
		;
	return 0;
}
