/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
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
 * wayrdp's glue: a Wayland client of the compositor on one side, the RDP
 * server on the other. The Wayland half is wayvnc's, the RDP half lives in
 * src/rdp/.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <aml.h>
#include <pixman.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include "virtual-keyboard-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"

#include "buffer.h"
#include "cfg.h"
#include "data-control.h"
#include "image-source.h"
#include "keyboard.h"
#include "log.h"
#include "observer.h"
#include "option-parser.h"
#include "output.h"
#include "pointer.h"
#include "rdp/server.h"
#include "rdp/auth.h"
#include "rdp/keymap.h"
#include "screencopy-interface.h"
#include "seat.h"
#include "strlcpy.h"
#include "time-util.h"
#include "transform-util.h"
#include "version.h"
#include "wayland.h"

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 3389
#define DEFAULT_MAX_RATE 30

struct wayrdp {
	bool do_exit;
	int exit_code;

	struct cfg cfg;
	struct rdp_server* rdp;

	const char* output_name;
	char kb_buffer[128];
	const char* kb_layout;
	const char* kb_variant;
	bool overlay_cursor;
	int max_rate;

	/* --probe: capture this many frames, report and exit. */
	int probe_frames;
	int probe_done;
	uint64_t probe_damage;

	struct image_source* image_source;
	struct screencopy* screencopy;
	bool capturing;

	struct seat* seat;
	struct keyboard keyboard;
	struct pointer pointer;
	struct data_control data_control;
	bool have_input;
	bool have_data_control;

	struct wv_buffer* next_frame;
	uint64_t last_send_time;
	struct aml_timer* rate_limiter;
	struct aml_timer* capture_retry_timer;

	struct observer output_added_observer;
	struct observer output_removed_observer;
	struct observer wayland_destroy_observer;
	struct observer power_change_observer;
	struct observer source_destroy_observer;
};

/* buffer.c, output.c and the capture code reach Wayland through this. */
struct wayland* wayland = NULL;

static void wayrdp_exit(struct wayrdp* self, int code)
{
	self->do_exit = true;
	self->exit_code = code;
}

/* ---- capture ---- */

static bool capture_wanted(const struct wayrdp* self)
{
	if (self->probe_frames > 0)
		return self->probe_done < self->probe_frames;
	return self->rdp && rdp_server_wants_frames(self->rdp);
}

static void start_capture(struct wayrdp* self, bool immediate)
{
	if (!self->screencopy || self->capture_retry_timer)
		return;

	if (immediate) {
		int rc = image_source_acquire_power_on(self->image_source);
		if (rc == 0) {
			wv_log(WV_LOG_DEBUG, "Waiting for the output to power on");
			return;
		} else if (rc > 0 && image_source_get_power(self->image_source)
				!= IMAGE_SOURCE_POWER_ON) {
			wv_log(WV_LOG_DEBUG, "Output not powered on yet");
			return;
		} else if (rc < 0) {
			wv_log(WV_LOG_WARNING, "No power management for the output. Capturing may fail.");
		}
	}

	if (screencopy_start(self->screencopy, immediate) < 0) {
		wv_log(WV_LOG_ERROR, "Failed to start capture. Exiting...");
		wayrdp_exit(self, 1);
		return;
	}
	self->capturing = true;
}

static void stop_capture(struct wayrdp* self)
{
	if (!self->capturing)
		return;
	screencopy_stop(self->screencopy);
	if (self->image_source)
		image_source_release_power_on(self->image_source);
	self->capturing = false;
}

/* Starts or stops the capture after a client came or went. */
static void wayrdp_update_capture(struct wayrdp* self)
{
	if (capture_wanted(self)) {
		if (!self->capturing)
			start_capture(self, true);
	} else {
		stop_capture(self);
		if (self->next_frame) {
			wv_buffer_release(self->next_frame);
			self->next_frame = NULL;
		}
	}
}

static void on_wants_frames_changed(void* userdata)
{
	wayrdp_update_capture(userdata);
}

static void tell_desktop_size(struct wayrdp* self)
{
	int width = 0, height = 0;
	if (self->rdp && self->image_source && image_source_get_buffer_size(
				self->image_source, &width, &height))
		rdp_server_set_desktop_size(self->rdp, width, height);
}

static void on_capture_retry(struct aml_timer* timer)
{
	struct wayrdp* self = aml_get_userdata(timer);
	aml_unref(self->capture_retry_timer);
	self->capture_retry_timer = NULL;
	if (capture_wanted(self))
		start_capture(self, true);
}

static void retry_capture(struct wayrdp* self)
{
	if (self->capture_retry_timer)
		return;
	self->capture_retry_timer = aml_timer_new(100000, on_capture_retry,
			self, NULL);
	aml_start(aml_get_default(), self->capture_retry_timer);
}

/* Damage in the orientation of the picture the client sees. */
static void frame_damage(const struct wayrdp* self, struct wv_buffer* buffer,
		struct pixman_region16* damage)
{
	if (screencopy_get_capabilities(self->screencopy)
			& SCREENCOPY_CAP_TRANSFORM) {
		pixman_region_copy(damage, &buffer->frame_damage);
	} else if (buffer->y_inverted) {
		buffer->transform = wv_output_transform_compose(
				image_source_get_transform(self->image_source),
				WL_OUTPUT_TRANSFORM_FLIPPED_180);
		wv_region_transform(damage, &buffer->frame_damage,
				WL_OUTPUT_TRANSFORM_FLIPPED_180,
				buffer->width, buffer->height);
	} else {
		buffer->transform =
			image_source_get_transform(self->image_source);
		pixman_region_copy(damage, &buffer->frame_damage);
	}

	pixman_region_intersect_rect(damage, damage, 0, 0, buffer->width,
			buffer->height);
}

static uint64_t region_area(struct pixman_region16* region)
{
	int n = 0;
	const pixman_box16_t* box = pixman_region_rectangles(region, &n);
	uint64_t area = 0;
	for (int i = 0; i < n; ++i)
		area += (uint64_t)(box[i].x2 - box[i].x1) * (box[i].y2 - box[i].y1);
	return area;
}

static void probe_report(struct wayrdp* self, const struct wv_buffer* buffer)
{
	printf("%dx%d %.4s frames=%d damage=%"PRIu64"\n",
			buffer->width, buffer->height,
			(const char*)&buffer->format, self->probe_done,
			self->probe_damage);
	fflush(stdout);
}

static void send_next_frame(struct wayrdp* self, uint64_t now)
{
	struct wv_buffer* buffer = self->next_frame;
	self->next_frame = NULL;
	if (!buffer)
		return;

	struct pixman_region16 damage;
	pixman_region_init(&damage);
	frame_damage(self, buffer, &damage);

	if (self->rdp)
		rdp_frame_submit(self->rdp, buffer, &damage);
	self->probe_damage += region_area(&damage);
	pixman_region_fini(&damage);
	self->last_send_time = now;

	if (self->probe_frames > 0 && ++self->probe_done >= self->probe_frames) {
		probe_report(self, buffer);
		wv_buffer_release(buffer);
		stop_capture(self);
		wayrdp_exit(self, 0);
		return;
	}

	wv_buffer_release(buffer);

	if (capture_wanted(self))
		start_capture(self, false);
}

static void on_rate_limit(struct aml_timer* timer)
{
	struct wayrdp* self = aml_get_userdata(timer);
	send_next_frame(self, gettime_us());
}

static void process_frame(struct wayrdp* self, struct wv_buffer* buffer)
{
	bool have_pending = false;
	if (self->next_frame) {
		pixman_region_union(&buffer->frame_damage, &buffer->frame_damage,
				&self->next_frame->frame_damage);
		wv_buffer_release(self->next_frame);
		have_pending = true;
	}
	self->next_frame = buffer;

	if (have_pending)
		return;

	uint64_t now = gettime_us();
	double dt = (now - self->last_send_time) * 1.0e-6;
	int32_t time_left = (1.0 / self->max_rate - dt) * 1.0e6;

	if (time_left > 0) {
		aml_set_duration(self->rate_limiter, time_left);
		aml_start(aml_get_default(), self->rate_limiter);
	} else {
		send_next_frame(self, now);
	}
}

static void on_capture_done(enum screencopy_result result,
		struct wv_buffer* buffer, struct image_source* source,
		void* userdata)
{
	struct wayrdp* self = userdata;

	switch (result) {
	case SCREENCOPY_FATAL:
		wv_log(WV_LOG_ERROR, "Failed to capture image. The source probably went away");
		self->capturing = false;
		break;
	case SCREENCOPY_FAILED:
		self->capturing = false;
		retry_capture(self);
		break;
	case SCREENCOPY_DONE:
		process_frame(self, buffer);
		break;
	}
}

/* RemoteFX and Planar take 32-bit pixels; XRGB8888 needs no conversion. */
static double rate_output_format(const void* userdata,
		enum wv_buffer_type type, uint32_t format, uint64_t modifier)
{
	if (type != WV_BUFFER_SHM)
		return 0;
	switch (format) {
	case DRM_FORMAT_XRGB8888: return 1.0;
	case DRM_FORMAT_ARGB8888: return 0.9;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: return 0.5;
	}
	return 0;
}

static bool configure_screencopy(struct wayrdp* self)
{
	stop_capture(self);
	screencopy_destroy(self->screencopy);

	self->screencopy = screencopy_create(self->image_source,
			self->overlay_cursor);
	if (!self->screencopy) {
		wv_log(WV_LOG_ERROR, "The compositor offers no way to capture the screen");
		return false;
	}

	self->screencopy->on_done = on_capture_done;
	self->screencopy->rate_format = rate_output_format;
	self->screencopy->userdata = self;
	/* wlroots captures on the next commit, so the capture runs at twice
	 * the rate and the rate limiter above evens it out (as in wayvnc). */
	self->screencopy->rate_limit = self->max_rate * 2;
	self->screencopy->enable_linux_dmabuf = false;
	return true;
}

/* ---- input and clipboard ---- */

static void init_pointer(struct wayrdp* self)
{
	struct zwlr_virtual_pointer_manager_v1* manager =
		wayland->zwlr_virtual_pointer_manager_v1;

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);
	self->pointer.pointer = NULL;

	struct output* output = image_source_is_output(self->image_source)
		? output_from_image_source(self->image_source) : NULL;

	/* Version 2 maps absolute motion to one output. */
	self->pointer.pointer =
		output && zwlr_virtual_pointer_manager_v1_get_version(manager) >= 2
		? zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
				manager, self->seat->wl_seat, output->wl_output)
		: zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
				manager, self->seat->wl_seat);

	if (pointer_init(&self->pointer) < 0)
		wv_log(WV_LOG_ERROR, "Failed to initialise pointer");
}

static int init_keyboard(struct wayrdp* self)
{
	self->keyboard.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			wayland->zwp_virtual_keyboard_manager_v1,
			self->seat->wl_seat);

	struct xkb_rule_names rule_names = {
		.layout = self->kb_layout,
		.variant = self->kb_variant,
		.model = "pc105",
	};
	if (keyboard_init(&self->keyboard, &rule_names) < 0) {
		wv_log(WV_LOG_ERROR, "Failed to initialise keyboard");
		return -1;
	}
	return 0;
}

/* From the RDP side. evdev codes are xkb codes minus 8. */
static void on_input_key(void* userdata, uint32_t code, bool pressed)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		keyboard_feed_code(&self->keyboard, code + 8, pressed);
}

static void on_input_keysym(void* userdata, uint32_t keysym, bool pressed)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		keyboard_feed(&self->keyboard, keysym, pressed);
}

static void on_input_release_all(void* userdata)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		keyboard_release_all(&self->keyboard);
}

/* Presses a lock key once where the session differs from the client. */
static void on_input_sync_locks(void* userdata, uint32_t locks)
{
	struct wayrdp* self = userdata;
	if (!self->have_input)
		return;

	static const struct {
		uint32_t rdp;
		enum keyboard_led_state led;
		uint32_t key;
	} map[] = {
		{ RDP_SYNC_CAPS_LOCK, KEYBOARD_LED_CAPS_LOCK, KEY_CAPSLOCK },
		{ RDP_SYNC_NUM_LOCK, KEYBOARD_LED_NUM_LOCK, KEY_NUMLOCK },
		{ RDP_SYNC_SCROLL_LOCK, KEYBOARD_LED_SCROLL_LOCK, KEY_SCROLLLOCK },
	};

	enum keyboard_led_state leds = keyboard_get_led_state(&self->keyboard);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
		bool want = locks & map[i].rdp;
		bool have = leds & map[i].led;
		if (want == have)
			continue;
		keyboard_feed_code(&self->keyboard, map[i].key + 8, true);
		keyboard_feed_code(&self->keyboard, map[i].key + 8, false);
	}
}

static void on_input_motion(void* userdata, double x, double y)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		pointer_motion(&self->pointer, x, y);
}

static void on_input_button(void* userdata, uint32_t code, bool pressed)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		pointer_button(&self->pointer, code, pressed);
}

static void on_input_axis(void* userdata, bool horizontal, int32_t value120)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		pointer_axis(&self->pointer, horizontal ? POINTER_AXIS_HORIZONTAL
				: POINTER_AXIS_VERTICAL, value120);
}

static void on_input_frame(void* userdata)
{
	struct wayrdp* self = userdata;
	if (self->have_input)
		pointer_frame(&self->pointer);
}

static const struct rdp_input_handler input_handler = {
	.key = on_input_key,
	.keysym = on_input_keysym,
	.release_all = on_input_release_all,
	.sync_locks = on_input_sync_locks,
	.motion = on_input_motion,
	.button = on_input_button,
	.axis = on_input_axis,
	.frame = on_input_frame,
};

static void on_selection_text(void* userdata, const char* text, size_t len)
{
	struct wayrdp* self = userdata;
	if (self->rdp)
		rdp_server_set_selection_text(self->rdp, text, len);
}

static void init_data_control(struct wayrdp* self)
{
	if (wayland->ext_data_control_manager_v1)
		self->data_control.ext_manager =
			wayland->ext_data_control_manager_v1;
	else if (wayland->zwlr_data_control_manager_v1)
		self->data_control.wlr_manager =
			wayland->zwlr_data_control_manager_v1;
	else {
		wv_log(WV_LOG_WARNING, "No data-control protocol: no clipboard");
		return;
	}

	data_control_init(&self->data_control, self->seat->wl_seat,
			on_selection_text, self);
	self->have_data_control = true;
}

static void destroy_input(struct wayrdp* self)
{
	if (self->have_data_control)
		data_control_destroy(&self->data_control);
	self->have_data_control = false;

	if (self->have_input) {
		keyboard_destroy(&self->keyboard);
		pointer_destroy(&self->pointer);
	}
	memset(&self->keyboard, 0, sizeof(self->keyboard));
	memset(&self->pointer, 0, sizeof(self->pointer));
	self->have_input = false;
}

/* ---- image source and outputs ---- */

static void log_image_source(struct wayrdp* self)
{
	char description[256];
	image_source_describe(self->image_source, description,
			sizeof(description));
	wv_log(WV_LOG_INFO, "Capturing %s", description);
}

static void on_power_change(struct observer* observer, void* arg)
{
	struct wayrdp* self = wl_container_of(observer, self,
			power_change_observer);
	enum image_source_power_state state =
		image_source_get_power(self->image_source);

	wv_trace("Power state changed to %s",
			image_source_power_state_name(state));

	if (!capture_wanted(self))
		return;

	switch (state) {
	case IMAGE_SOURCE_POWER_ON:
		start_capture(self, true);
		break;
	case IMAGE_SOURCE_POWER_OFF:
		wv_log(WV_LOG_WARNING, "Output is now off. Pausing frame capture");
		screencopy_stop(self->screencopy);
		self->capturing = false;
		break;
	default:
		break;
	}
}

static void on_source_destroyed(struct observer* observer, void* arg)
{
	struct wayrdp* self = wl_container_of(observer, self,
			source_destroy_observer);
	observer_deinit(&self->power_change_observer);
	observer_deinit(&self->source_destroy_observer);
	self->image_source = NULL;
}

static void set_image_source(struct wayrdp* self, struct image_source* source)
{
	if (self->image_source) {
		observer_deinit(&self->power_change_observer);
		observer_deinit(&self->source_destroy_observer);
	}
	self->image_source = source;
	observer_init(&self->power_change_observer,
			&source->observable.power_change, on_power_change);
	observer_init(&self->source_destroy_observer,
			&source->observable.destroyed, on_source_destroyed);
	log_image_source(self);
}

static void switch_to_output(struct wayrdp* self, struct output* output)
{
	stop_capture(self);
	if (self->next_frame) {
		wv_buffer_release(self->next_frame);
		self->next_frame = NULL;
	}

	set_image_source(self, &output->image_source);
	tell_desktop_size(self);
	if (!configure_screencopy(self)) {
		wayrdp_exit(self, 1);
		return;
	}
	if (self->have_input)
		init_pointer(self);
	wayrdp_update_capture(self);
}

static void on_output_added(struct observer* observer, void* data)
{
	struct wayrdp* self = wl_container_of(observer, self,
			output_added_observer);
	struct output* output = data;

	/* The output we were told to capture is back. */
	if (self->output_name && strcmp(output_get_name(output),
				self->output_name) == 0)
		switch_to_output(self, output);
	else if (!self->image_source)
		switch_to_output(self, output);
}

static void on_output_removed(struct observer* observer, void* data)
{
	struct wayrdp* self = wl_container_of(observer, self,
			output_removed_observer);
	struct output* output = data;

	if (!self->image_source || !image_source_is_output(self->image_source)
			|| output_from_image_source(self->image_source) != output) {
		wv_log(WV_LOG_INFO, "Output %s went away", output_get_name(output));
		return;
	}

	wv_log(WV_LOG_WARNING, "Captured output %s went away",
			output_get_name(output));

	stop_capture(self);
	screencopy_destroy(self->screencopy);
	self->screencopy = NULL;
	if (self->next_frame) {
		wv_buffer_release(self->next_frame);
		self->next_frame = NULL;
	}
	observer_deinit(&self->power_change_observer);
	observer_deinit(&self->source_destroy_observer);
	self->image_source = NULL;

	struct output* other;
	wl_list_for_each(other, &wayland->outputs, link) {
		if (other != output) {
			switch_to_output(self, other);
			return;
		}
	}
	wv_log(WV_LOG_WARNING, "No output left to capture. Waiting for one");
}

static void on_wayland_destroyed(struct observer* observer, void* data)
{
	struct wayrdp* self = wl_container_of(observer, self,
			wayland_destroy_observer);

	wv_log(WV_LOG_INFO, "The compositor went away");

	observer_deinit(&self->output_added_observer);
	observer_deinit(&self->output_removed_observer);
	observer_deinit(&self->wayland_destroy_observer);
	if (self->image_source) {
		observer_deinit(&self->power_change_observer);
		observer_deinit(&self->source_destroy_observer);
	}

	if (self->next_frame) {
		wv_buffer_release(self->next_frame);
		self->next_frame = NULL;
	}
	screencopy_stop(self->screencopy);
	screencopy_destroy(self->screencopy);
	self->screencopy = NULL;
	self->capturing = false;
	destroy_input(self);
	self->image_source = NULL;

	wayland = NULL;
	wayrdp_exit(self, 0);
}

static int attach_wayland(struct wayrdp* self)
{
	wayland = wayland_connect(NULL, WAYLAND_FLAG_ENABLE_INPUT);
	if (!wayland) {
		wv_log(WV_LOG_ERROR, "Failed to connect to the compositor. Is WAYLAND_DISPLAY set?");
		return -1;
	}

	if (!wayland->zwlr_screencopy_manager_v1 &&
			!wayland->ext_image_copy_capture_manager_v1) {
		wv_log(WV_LOG_ERROR, "The compositor offers no screen capture protocol");
		goto failure;
	}

	if (!wayland->zwlr_virtual_pointer_manager_v1 ||
			!wayland->zwp_virtual_keyboard_manager_v1) {
		wv_log(WV_LOG_ERROR, "The compositor offers no virtual keyboard or pointer. For mwc, start it with MWC_VIRTUAL_INPUT=1");
		goto failure;
	}

	struct output* output;
	if (self->output_name) {
		output = output_find_by_name(&wayland->outputs,
				self->output_name);
		if (!output) {
			wv_log(WV_LOG_ERROR, "No such output: %s",
					self->output_name);
			goto failure;
		}
	} else {
		output = output_first(&wayland->outputs);
		if (!output) {
			wv_log(WV_LOG_ERROR, "No output found");
			goto failure;
		}
	}

	self->seat = seat_first(&wayland->seats);
	if (!self->seat) {
		wv_log(WV_LOG_ERROR, "The compositor has no seat");
		goto failure;
	}

	observer_init(&self->output_added_observer,
			&wayland->observable.output_added, on_output_added);
	observer_init(&self->output_removed_observer,
			&wayland->observable.output_removed, on_output_removed);
	observer_init(&self->wayland_destroy_observer,
			&wayland->observable.destroyed, on_wayland_destroyed);

	set_image_source(self, &output->image_source);
	if (!configure_screencopy(self))
		goto failure;

	if (init_keyboard(self) < 0)
		goto failure;
	init_pointer(self);
	self->have_input = true;
	init_data_control(self);

	return 0;

failure:
	destroy_input(self);
	wayland_destroy(wayland);
	wayland = NULL;
	return -1;
}

static void detach_wayland(struct wayrdp* self)
{
	if (!wayland)
		return;
	/* Fires on_wayland_destroyed, which cleans up after us. */
	wayland_destroy(wayland);
	wayland = NULL;
}

/* ---- options and config ---- */

static void on_signal(struct aml_signal* sig)
{
	struct wayrdp* self = aml_get_userdata(sig);
	wv_log(WV_LOG_INFO, "Received termination signal");
	wayrdp_exit(self, 0);
}

static int add_signal(struct wayrdp* self, int signo)
{
	struct aml_signal* sig = aml_signal_new(signo, on_signal, self, NULL);
	if (!sig)
		return -1;
	int rc = aml_start(aml_get_default(), sig);
	aml_unref(sig);
	return rc;
}

/* "de" or "de-nodeadkeys": layout and optional variant. */
static void parse_keyboard(struct wayrdp* self, const char* arg)
{
	strlcpy(self->kb_buffer, arg, sizeof(self->kb_buffer));
	self->kb_layout = self->kb_buffer;
	self->kb_variant = NULL;
	char* dash = strchr(self->kb_buffer, '-');
	if (dash) {
		*dash = '\0';
		self->kb_variant = dash + 1;
	}
}

static int usage(struct option_parser* parser, FILE* stream, int rc)
{
	fprintf(stream, "Usage: wayrdp");
	option_parser_print_usage(parser, stream);
	fprintf(stream, "\n");
	option_parser_print_cmd_summary(
			"Starts an RDP server for $WAYLAND_DISPLAY", stream);
	if (option_parser_print_arguments(parser, stream))
		fprintf(stream, "\n");
	option_parser_print_options(parser, stream);
	fprintf(stream, "\n");
	return rc;
}

int main(int argc, char* argv[])
{
	struct wayrdp self = { 0 };
	int rc = 1;

	static const struct wv_option opts[] = {
		{ .positional = "address",
		  .help = "The address to listen on.",
		  .default_ = DEFAULT_ADDRESS },
		{ .positional = "port",
		  .help = "The port to listen on.",
		  .default_ = "3389" },
		{ 'a', "auth", "nla|pam",
		  "Login: NLA against the password from --set-password (default), or PAM (M5)." },
		{ 'C', "config", "<path>",
		  "Select a config file. Default: ~/.config/wayrdp/config" },
		{ 'f', "max-fps", "<fps>",
		  "Set the highest frame rate." },
		{ 'h', "help", NULL,
		  "Get help (this text)." },
		{ 'k', "keyboard", "<layout>[-<variant>]",
		  "Keyboard layout, e.g. de or de-nodeadkeys." },
		{ 'L', "log-level", "<level>",
		  "Set log level: error, warning, info, debug, trace or quiet.",
		  .default_ = "warning" },
		{ 'o', "output", "<name>",
		  "Select the output to capture, e.g. HEADLESS-1." },
		{ 0, "probe", "<frames>",
		  "Capture this many frames without a client, print size and damage, exit." },
		{ 'r', "render-cursor", NULL,
		  "Draw the pointer into the picture." },
		{ 0, "set-password", NULL,
		  "Set the RDP password for nla and exit." },
		{ 'v', NULL, NULL,
		  "Print the version and exit." },
		{}
	};

	struct option_parser parser;
	option_parser_init(&parser, opts);
	if (option_parser_parse(&parser, argc, (const char* const*)argv) < 0)
		return usage(&parser, stderr, 1);

	if (option_parser_get_value(&parser, "v")) {
		printf("wayrdp %s\n", WAYRDP_VERSION);
		return 0;
	}
	if (option_parser_get_value(&parser, "help"))
		return usage(&parser, stdout, 0);

	int log_level = wv_log_level_from_string(
			option_parser_get_value(&parser, "log-level"));
	if (log_level < 0) {
		wv_log(WV_LOG_ERROR, "Invalid log level: %s",
				option_parser_get_value(&parser, "log-level"));
		return 1;
	}
	wv_log_set_level(log_level);

	const char* cfg_file = option_parser_get_value(&parser, "config");
	errno = 0;
	int cfg_rc = cfg_load(&self.cfg, cfg_file);
	if (cfg_rc != 0 && (cfg_file || errno != ENOENT)) {
		if (cfg_rc > 0)
			wv_log(WV_LOG_ERROR, "Failed to load config. Error on line %d",
					cfg_rc);
		else
			wv_log(WV_LOG_ERROR, "Failed to load config: %m");
		return 1;
	}

	const char* config_dir = rdp_config_dir();
	char default_sam[4200], default_crt[4200], default_key[4200];
	snprintf(default_sam, sizeof(default_sam), "%s/sam",
			config_dir ? config_dir : ".");
	snprintf(default_crt, sizeof(default_crt), "%s/tls.crt",
			config_dir ? config_dir : ".");
	snprintf(default_key, sizeof(default_key), "%s/tls.key",
			config_dir ? config_dir : ".");
	const char* sam_file = self.cfg.sam_file ? self.cfg.sam_file : default_sam;

	if (option_parser_get_value(&parser, "set-password")) {
		rc = rdp_auth_set_password(sam_file);
		goto out;
	}

	const char* auth = option_parser_get_value(&parser, "auth");
	if (!auth)
		auth = self.cfg.auth ? self.cfg.auth : "nla";
	if (strcmp(auth, "pam") == 0) {
		wv_log(WV_LOG_ERROR, "auth=pam comes with M5; use nla");
		goto out;
	} else if (strcmp(auth, "nla") != 0) {
		wv_log(WV_LOG_ERROR, "Unknown auth mode: %s", auth);
		goto out;
	}

	/* The command line wins over the config file. */
	const char* address = option_parser_get_value_no_default(&parser,
			"address");
	if (!address)
		address = self.cfg.address ? self.cfg.address : DEFAULT_ADDRESS;

	int port = self.cfg.port ? (int)self.cfg.port : DEFAULT_PORT;
	const char* port_str = option_parser_get_value_no_default(&parser,
			"port");
	if (port_str) {
		char* end;
		long p = strtol(port_str, &end, 10);
		if (!*port_str || *end || p <= 0 || p > 65535) {
			wv_log(WV_LOG_ERROR, "Invalid port: %s", port_str);
			goto out;
		}
		port = p;
	}

	self.max_rate = self.cfg.max_rate ? (int)self.cfg.max_rate
		: DEFAULT_MAX_RATE;
	const char* max_fps = option_parser_get_value(&parser, "max-fps");
	if (max_fps)
		self.max_rate = atoi(max_fps);
	if (self.max_rate <= 0) {
		wv_log(WV_LOG_ERROR, "Invalid frame rate");
		goto out;
	}

	const char* keyboard = option_parser_get_value(&parser, "keyboard");
	if (!keyboard)
		keyboard = self.cfg.keyboard;
	if (keyboard)
		parse_keyboard(&self, keyboard);

	self.output_name = option_parser_get_value(&parser, "output");
	if (!self.output_name)
		self.output_name = self.cfg.output;
	self.overlay_cursor = !!option_parser_get_value(&parser,
			"render-cursor");

	const char* probe = option_parser_get_value(&parser, "probe");
	if (probe) {
		self.probe_frames = atoi(probe);
		if (self.probe_frames <= 0) {
			wv_log(WV_LOG_ERROR, "Invalid number of frames: %s", probe);
			goto out;
		}
	}

	if (strcmp(address, "127.0.0.1") != 0 && strcmp(address, "::1") != 0
			&& strcmp(address, "localhost") != 0)
		wv_log(WV_LOG_WARNING, "Listening on %s, not only on localhost",
				address);

	signal(SIGPIPE, SIG_IGN);
	srand(time(NULL));

	struct aml* aml = aml_new();
	if (!aml)
		goto out;
	aml_set_default(aml);

	if (add_signal(&self, SIGINT) < 0 || add_signal(&self, SIGTERM) < 0)
		goto aml_failure;

	self.rate_limiter = aml_timer_new(0, on_rate_limit, &self, NULL);
	if (!self.rate_limiter)
		goto aml_failure;

	struct rdp_server_config rdp_config = {
		.address = address,
		.port = port,
		.certificate_file = self.cfg.certificate_file
			? self.cfg.certificate_file : default_crt,
		.private_key_file = self.cfg.private_key_file
			? self.cfg.private_key_file : default_key,
		.sam_file = sam_file,
		.on_wants_frames_changed = on_wants_frames_changed,
		.input = &input_handler,
		.userdata = &self,
	};

	/* Connect to the compositor first: a client may come at once. */
	if (attach_wayland(&self) < 0)
		goto wayland_failure;

	if (!self.probe_frames) {
		self.rdp = rdp_server_new(&rdp_config);
		if (!self.rdp)
			goto rdp_failure;
		tell_desktop_size(&self);
	}

	wayrdp_update_capture(&self);

	wl_display_dispatch_pending(wayland->display);
	while (!self.do_exit) {
		if (wayland)
			wl_display_flush(wayland->display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}
	rc = self.exit_code;

	wv_log(WV_LOG_INFO, "Exiting...");

	rdp_server_destroy(self.rdp);
	self.rdp = NULL;
rdp_failure:
	detach_wayland(&self);
wayland_failure:
	if (self.capture_retry_timer) {
		aml_stop(aml, self.capture_retry_timer);
		aml_unref(self.capture_retry_timer);
	}
	aml_stop(aml, self.rate_limiter);
	aml_unref(self.rate_limiter);
aml_failure:
	aml_unref(aml);
out:
	cfg_destroy(&self.cfg);
	return rc;
}
