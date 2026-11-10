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
 * wayrdp's logging, in place of neatvnc's nvnc_log(). Same shape, so the code
 * kept from wayvnc only needed its names changed.
 */

#pragma once

#include <stdbool.h>

enum wv_log_level {
	WV_LOG_PANIC = 0,
	WV_LOG_ERROR,
	WV_LOG_WARNING,
	WV_LOG_INFO,
	WV_LOG_DEBUG,
	WV_LOG_TRACE,
};

void wv__log(enum wv_log_level level, const char* file, int line,
		const char* fmt, ...) __attribute__((format(printf, 4, 5)));

#define wv_log(level, fmt, ...) \
	wv__log(level, __FILE__, __LINE__, fmt __VA_OPT__(,) __VA_ARGS__)

#ifdef NDEBUG
#define wv_trace(...)
#else
#define wv_trace(fmt, ...) wv_log(WV_LOG_TRACE, fmt __VA_OPT__(,) __VA_ARGS__)
#endif

void wv_log_set_level(enum wv_log_level level);
enum wv_log_level wv_log_get_level(void);
bool wv_log_is_enabled(enum wv_log_level level);

/* "error", "warning", …, "quiet". -1 for anything else. */
int wv_log_level_from_string(const char* str);
