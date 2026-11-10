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
 * Clipboard text between RDP and Wayland: CF_UNICODETEXT is UTF-16LE with
 * CRLF line ends and a terminating NUL, Wayland text is UTF-8 with LF.
 * No FreeRDP in here (test/text-test.c).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* The largest clipboard text either way, in bytes of the source. */
#define RDP_CLIPBOARD_MAX (16u * 1024 * 1024)

/* UTF-16LE (up to the first NUL, or len bytes) to UTF-8, CRLF to LF.
 * Broken surrogates become U+FFFD. Returns a malloc'd, NUL-terminated
 * string and its length in *out_len, or NULL. */
char* rdp_utf16le_to_utf8(const uint8_t* data, size_t len, size_t* out_len);

/* UTF-8 to UTF-16LE with a terminating NUL, LF to CRLF (a CR already in
 * front stays alone). Broken sequences become U+FFFD. Returns malloc'd
 * bytes and their number, NUL included, in *out_len, or NULL. */
uint8_t* rdp_utf8_to_utf16le(const char* text, size_t len, size_t* out_len);
