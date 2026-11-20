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


/* The self-signed certificate wayrdp makes on its first start. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tst.h"
#include "rdp/auth.h"

static char dir[] = "/tmp/wayrdp-auth-test.XXXXXX";
static char cert[4096], key[4096];

static bool exists(const char* path)
{
	struct stat st;
	return lstat(path, &st) == 0;
}

/* Made with mode 0600; there, it isn't made again. */
static int test_made(void)
{
	ASSERT_INT_EQ(0, rdp_auth_ensure_certificate(cert, key));
	struct stat st;
	ASSERT_INT_EQ(0, stat(key, &st));
	ASSERT_INT_EQ(0600, (int)(st.st_mode & 0777));
	ASSERT_INT_EQ(0, stat(cert, &st));
	time_t made = st.st_mtime;
	ASSERT_INT_EQ(0, rdp_auth_ensure_certificate(cert, key));
	ASSERT_INT_EQ(0, stat(cert, &st));
	ASSERT_TRUE(st.st_mtime == made);
	unlink(cert);
	unlink(key);
	return 0;
}

/* Writing the key fails (/dev/full: ENOSPC): neither file stays, so the
 * next start makes both again instead of taking a broken key. */
static int test_write_fails(void)
{
	ASSERT_INT_EQ(0, symlink("/dev/full", key));
	ASSERT_INT_EQ(-1, rdp_auth_ensure_certificate(cert, key));
	ASSERT_FALSE(exists(cert));
	ASSERT_FALSE(exists(key));
	return 0;
}

/* A certificate of the user's that can't be written, the key missing: the
 * new pair can't go there, and the certificate it didn't write stays. */
static int test_keeps_unwritten(void)
{
	FILE* f = fopen(cert, "w");
	ASSERT_TRUE(f != NULL);
	fputs("mine\n", f);
	fclose(f);
	ASSERT_INT_EQ(0, chmod(cert, 0400));
	ASSERT_INT_EQ(-1, rdp_auth_ensure_certificate(cert, key));
	ASSERT_TRUE(exists(cert));
	char buf[16] = "";
	f = fopen(cert, "r");
	ASSERT_TRUE(f != NULL);
	ASSERT_TRUE(fgets(buf, sizeof(buf), f) != NULL);
	fclose(f);
	ASSERT_INT_EQ(0, strcmp(buf, "mine\n"));
	chmod(cert, 0600);
	unlink(cert);
	unlink(key);
	return 0;
}

int main(void)
{
	if (!mkdtemp(dir))
		return 1;
	snprintf(cert, sizeof(cert), "%s/tls.crt", dir);
	snprintf(key, sizeof(key), "%s/tls.key", dir);
	int r = 0;
	RUN_TEST(test_made);
	RUN_TEST(test_write_fails);
	RUN_TEST(test_keeps_unwritten);
	unlink(cert);
	unlink(key);
	rmdir(dir);
	return r;
}
