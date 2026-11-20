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


#include <stdatomic.h>
#include <time.h>

#include "tst.h"
#include "rdp/deadline.h"

static atomic_int fired;
static atomic_ullong fired_at;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void fire(void* arg)
{
	fired_at = now_us();
	atomic_fetch_add(&fired, 1);
}

/* Up, it fires once, on its own thread, not before the time. */
static int test_fires(void)
{
	fired = 0;
	uint64_t start = now_us();
	struct rdp_deadline* d = rdp_deadline_start(50000, fire, NULL);
	ASSERT_TRUE(d != NULL);
	/* Polled, with a bound far above the 50 ms. */
	for (int i = 0; i < 500 && !fired; ++i) {
		struct timespec ts = { 0, 10 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	ASSERT_INT_EQ(1, (int)fired);
	ASSERT_TRUE(fired_at - start >= 50000);
	ASSERT_TRUE(rdp_deadline_fired(d));
	ASSERT_TRUE(rdp_deadline_stop(d));
	ASSERT_INT_EQ(1, (int)fired);
	return 0;
}

/* Stopped before: it doesn't fire, and stop doesn't wait for the time. */
static int test_stopped(void)
{
	fired = 0;
	uint64_t start = now_us();
	struct rdp_deadline* d = rdp_deadline_start(10 * 1000000, fire, NULL);
	ASSERT_TRUE(d != NULL);
	ASSERT_FALSE(rdp_deadline_fired(d));
	ASSERT_FALSE(rdp_deadline_stop(d));
	ASSERT_TRUE(now_us() - start < 1000000);
	ASSERT_INT_EQ(0, (int)fired);
	return 0;
}

static int test_stop_null(void)
{
	ASSERT_FALSE(rdp_deadline_stop(NULL));
	ASSERT_FALSE(rdp_deadline_fired(NULL));
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_fires);
	RUN_TEST(test_stopped);
	RUN_TEST(test_stop_null);
	return r;
}
