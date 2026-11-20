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


#include "tst.h"
#include "time-util.h"

static int test_rate_wait(void)
{
	/* 30 a second: 33333 µs apart. */
	ASSERT_UINT32_EQ(33333, rate_wait_us(1000000, 1000000, 30));
	ASSERT_UINT32_EQ(23333, rate_wait_us(1000000, 1010000, 30));
	ASSERT_UINT32_EQ(0, rate_wait_us(1000000, 1033334, 30));
	return 0;
}

/* The last frame long ago (or never, at 0): may go now, without the wait
 * overflowing an int32_t (UBSan). */
static int test_long_ago(void)
{
	ASSERT_UINT32_EQ(0, rate_wait_us(0, UINT64_C(3000) * 1000000, 30));
	ASSERT_UINT32_EQ(0, rate_wait_us(0, UINT64_C(86400) * 365 * 1000000, 1));
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_rate_wait);
	RUN_TEST(test_long_ago);
	return r;
}
