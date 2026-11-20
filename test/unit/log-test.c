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


/* wv_log writes one line per message, whatever the arguments hold. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tst.h"
#include "log.h"

static char got[8192];

/* Runs the log call with stderr in a real file and reads it back. */
#define CAPTURE(call) do { \
	FILE* f = tmpfile(); \
	ASSERT_TRUE(f != NULL); \
	fflush(stderr); \
	int saved = dup(2); \
	dup2(fileno(f), 2); \
	call; \
	fflush(stderr); \
	dup2(saved, 2); \
	close(saved); \
	rewind(f); \
	size_t n = fread(got, 1, sizeof(got) - 1, f); \
	got[n] = '\0'; \
	fclose(f); \
} while (0)

static int count_lines(const char* s)
{
	int n = 0;
	for (; *s; ++s)
		n += *s == '\n';
	return n;
}

/* A user name from a client with a line of its own in it. */
static int test_newline(void)
{
	CAPTURE(wv_log(WV_LOG_WARNING, "%s: user %s refused", "127.0.0.1",
			"x\nInfo: 127.0.0.1: logged on as root (PAM)"));
	ASSERT_INT_EQ(1, count_lines(got));
	ASSERT_TRUE(strstr(got, "user x\\x0aInfo: 127.0.0.1") != NULL);
	return 0;
}

static int test_controls(void)
{
	CAPTURE(wv_log(WV_LOG_WARNING, "a%sb", "\r\t\x1b[31m\x7f"));
	ASSERT_INT_EQ(1, count_lines(got));
	ASSERT_TRUE(strstr(got, "a\\x0d\\x09\\x1b[31m\\x7fb") != NULL);
	return 0;
}

/* UTF-8 stays as it is. */
static int test_utf8(void)
{
	CAPTURE(wv_log(WV_LOG_WARNING, "Grüße %s", "– 日本"));
	ASSERT_TRUE(strstr(got, "Grüße – 日本\n") != NULL);
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_newline);
	RUN_TEST(test_controls);
	RUN_TEST(test_utf8);
	return r;
}
