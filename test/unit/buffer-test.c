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
 * Shared-memory buffers without a compositor: nothing may get past a
 * failed mmap. A real one hardly fails, so -Wl,--wrap=mmap64 makes it
 * (mmap64: meson builds with _FILE_OFFSET_BITS=64).
 */

#include <errno.h>
#include <sys/mman.h>
#include <libdrm/drm_fourcc.h>

#include "tst.h"
#include "buffer.h"
#include "wayland.h"

/* No compositor. buffer.c asserts there is a wl_shm, and after mmap it
 * would use it: with mmap failing, this one is never touched. */
static struct wayland fake_wayland = {
	.wl_shm = (struct wl_shm*)&fake_wayland,
};
struct wayland* wayland = &fake_wayland;

static int mmap_fails;

void* __real_mmap64(void* addr, size_t len, int prot, int flags, int fd,
		off_t off);

void* __wrap_mmap64(void* addr, size_t len, int prot, int flags, int fd,
		off_t off)
{
	if (mmap_fails) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return __real_mmap64(addr, len, prot, flags, fd, off);
}

static int test_mmap_fails(void)
{
	struct wv_buffer_config config = {
		.type = WV_BUFFER_SHM,
		.width = 64,
		.height = 64,
		.stride = 256,
		.format = DRM_FORMAT_XRGB8888,
	};
	struct wv_buffer_pool* pool = wv_buffer_pool_create(&config);
	ASSERT_TRUE(pool != NULL);
	mmap_fails = 1;
	ASSERT_TRUE(wv_buffer_pool_acquire(pool) == NULL);
	mmap_fails = 0;
	wv_buffer_pool_destroy(pool);
	return 0;
}

int main(void)
{
	int r = 0;
	RUN_TEST(test_mmap_fails);
	return r;
}
