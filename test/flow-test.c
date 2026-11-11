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
#include "rdp/flow.h"

/* Without ids nothing is ever waited for. */
static int test_no_acks(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, false);
	for (int i = 0; i < 10; ++i)
		rdp_flow_next_id(&f);
	ASSERT_INT_EQ(0, (int)rdp_flow_in_flight(&f));
	ASSERT_FALSE(rdp_flow_must_wait(&f));
	return 0;
}

/* Two frames on their way, then wait; an ack frees one. */
static int test_window(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, true);
	ASSERT_INT_EQ(1, (int)rdp_flow_next_id(&f));
	ASSERT_FALSE(rdp_flow_must_wait(&f));
	ASSERT_INT_EQ(2, (int)rdp_flow_next_id(&f));
	ASSERT_TRUE(rdp_flow_must_wait(&f));
	rdp_flow_ack(&f, 1);
	ASSERT_FALSE(rdp_flow_must_wait(&f));
	ASSERT_INT_EQ(1, (int)rdp_flow_in_flight(&f));
	rdp_flow_ack(&f, 2);
	ASSERT_INT_EQ(0, (int)rdp_flow_in_flight(&f));
	return 0;
}

/* No ack ever: the timeout switches the waiting off for good. */
static int test_never_acked(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, true);
	rdp_flow_next_id(&f);
	rdp_flow_next_id(&f);
	ASSERT_TRUE(rdp_flow_must_wait(&f));
	ASSERT_INT_EQ(RDP_FLOW_NEVER_ACKED, rdp_flow_timeout(&f));
	for (int i = 0; i < 10; ++i)
		rdp_flow_next_id(&f);
	ASSERT_FALSE(rdp_flow_must_wait(&f));
	return 0;
}

/* After acks came, a missing one counts as lost; waiting stays on. */
static int test_ack_lost(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, true);
	rdp_flow_next_id(&f);
	rdp_flow_ack(&f, 1);
	rdp_flow_next_id(&f);
	rdp_flow_next_id(&f);
	ASSERT_TRUE(rdp_flow_must_wait(&f));
	ASSERT_INT_EQ(RDP_FLOW_ACK_LOST, rdp_flow_timeout(&f));
	ASSERT_INT_EQ(0, (int)rdp_flow_in_flight(&f));
	rdp_flow_next_id(&f);
	rdp_flow_next_id(&f);
	ASSERT_TRUE(rdp_flow_must_wait(&f));
	return 0;
}

/* The pipeline's SUSPEND: no waiting, reported once. */
static int test_suspend(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, true);
	ASSERT_TRUE(rdp_flow_suspend(&f));
	ASSERT_FALSE(rdp_flow_suspend(&f));
	for (int i = 0; i < 10; ++i)
		rdp_flow_next_id(&f);
	ASSERT_FALSE(rdp_flow_must_wait(&f));
	return 0;
}

/* A new surface: ids go on, nothing counts as on its way; a reset starts
 * the ids again. */
static int test_continue_and_reset(void)
{
	struct rdp_flow f;
	rdp_flow_reset(&f, true);
	rdp_flow_next_id(&f);
	rdp_flow_next_id(&f);
	rdp_flow_continue(&f);
	ASSERT_INT_EQ(0, (int)rdp_flow_in_flight(&f));
	ASSERT_INT_EQ(3, (int)rdp_flow_next_id(&f));
	rdp_flow_reset(&f, true);
	ASSERT_INT_EQ(1, (int)rdp_flow_next_id(&f));
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_no_acks);
	RUN_TEST(test_window);
	RUN_TEST(test_never_acked);
	RUN_TEST(test_ack_lost);
	RUN_TEST(test_suspend);
	RUN_TEST(test_continue_and_reset);
	return r;
}
