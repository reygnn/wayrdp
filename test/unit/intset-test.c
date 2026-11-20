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
#include "intset.h"

static int test_set_clear(void)
{
	struct intset s;
	ASSERT_INT_EQ(0, intset_init(&s, 0));
	ASSERT_FALSE(intset_is_set(&s, 30));
	ASSERT_INT_EQ(0, intset_set(&s, 30));
	ASSERT_INT_EQ(0, intset_set(&s, 30));
	ASSERT_TRUE(intset_is_set(&s, 30));
	ASSERT_INT_EQ(1, (int)s.len);
	intset_clear(&s, 30);
	ASSERT_FALSE(intset_is_set(&s, 30));
	intset_destroy(&s);
	return 0;
}

/* Past the capacity the storage grows by elements, not bytes (ASan). */
static int test_grow(void)
{
	struct intset s;
	ASSERT_INT_EQ(0, intset_init(&s, 4));
	for (int32_t i = 0; i < 1000; ++i)
		ASSERT_INT_EQ(0, intset_set(&s, i));
	for (int32_t i = 0; i < 1000; ++i)
		ASSERT_TRUE(intset_is_set(&s, i));
	ASSERT_INT_EQ(1000, (int)s.len);
	intset_destroy(&s);
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_set_clear);
	RUN_TEST(test_grow);
	return r;
}
