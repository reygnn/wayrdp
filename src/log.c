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

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "log.h"

static enum wv_log_level log_level = WV_LOG_WARNING;

static const char* level_name(enum wv_log_level level)
{
	switch (level) {
	case WV_LOG_PANIC: return "PANIC";
	case WV_LOG_ERROR: return "ERROR";
	case WV_LOG_WARNING: return "Warning";
	case WV_LOG_INFO: return "Info";
	case WV_LOG_DEBUG: return "DEBUG";
	case WV_LOG_TRACE: return "TRACE";
	}
	return "?";
}

void wv_log_set_level(enum wv_log_level level)
{
	log_level = level;
}

enum wv_log_level wv_log_get_level(void)
{
	return log_level;
}

bool wv_log_is_enabled(enum wv_log_level level)
{
	return level <= log_level;
}

void wv__log(enum wv_log_level level, const char* file, int line,
		const char* fmt, ...)
{
	/* %m in fmt means the errno of the caller, not of what runs here. */
	int saved_errno = errno;

	if (level != WV_LOG_PANIC && !wv_log_is_enabled(level))
		return;

	const char* base = strrchr(file, '/');
	base = base ? base + 1 : file;

	char message[1024];
	va_list ap;
	va_start(ap, fmt);
	errno = saved_errno;
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	if (level >= WV_LOG_DEBUG)
		fprintf(stderr, "%s: %s:%d: %s\n", level_name(level), base,
				line, message);
	else
		fprintf(stderr, "%s: %s\n", level_name(level), message);

	if (level == WV_LOG_PANIC)
		abort();

	errno = saved_errno;
}

int wv_log_level_from_string(const char* str)
{
	if (strcmp(str, "quiet") == 0) return WV_LOG_PANIC;
	if (strcmp(str, "error") == 0) return WV_LOG_ERROR;
	if (strcmp(str, "warning") == 0) return WV_LOG_WARNING;
	if (strcmp(str, "info") == 0) return WV_LOG_INFO;
	if (strcmp(str, "debug") == 0) return WV_LOG_DEBUG;
	if (strcmp(str, "trace") == 0) return WV_LOG_TRACE;
	return -1;
}
