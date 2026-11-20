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
 * rdp_auth_pam against a PAM configuration of the test's own
 * (pam_start_confdir): pam_exec runs a script that writes down what PAM
 * was given, the password through the conversation (expose_authtok).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>

#include "tst.h"
#include "rdp/pam.h"

static char dir[] = "/tmp/wayrdp-pam-test.XXXXXX";

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static char out[4096];

static int write_file(const char* name, const char* text, mode_t mode)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(text, f);
	fclose(f);
	return chmod(path, mode);
}

/* What the script wrote, or "" */
static const char* dumped(void)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s/dump", dir);
	FILE* f = fopen(path, "r");
	out[0] = '\0';
	if (f) {
		size_t n = fread(out, 1, sizeof(out) - 1, f);
		out[n] = '\0';
		fclose(f);
	}
	return out;
}

static int service(const char* text)
{
	return write_file("wayrdp", text, 0644);
}

/* The items a login from elsewhere has, and the password PAM asked for. */
static int test_items(void)
{
	char conf[8192];
	snprintf(conf, sizeof(conf),
		"auth required pam_exec.so expose_authtok %s/dump.sh\n"
		"auth required pam_permit.so\n"
		"account required pam_permit.so\n", dir);
	ASSERT_INT_EQ(0, service(conf));
	ASSERT_INT_EQ(RDP_PAM_OK, rdp_auth_pam(dir, "someone", "secret 1", "192.0.2.7"));
	const char* d = dumped();
	ASSERT_TRUE(strstr(d, "rhost=192.0.2.7\n") != NULL);
	ASSERT_TRUE(strstr(d, "ruser=someone\n") != NULL);
	ASSERT_TRUE(strstr(d, "tty=wayrdp\n") != NULL);
	ASSERT_TRUE(strstr(d, "user=someone\n") != NULL);
	ASSERT_TRUE(strstr(d, "password=secret 1\n") != NULL);
	return 0;
}

static int test_refused(void)
{
	ASSERT_INT_EQ(0, service("auth required pam_deny.so\n"
				"account required pam_permit.so\n"));
	ASSERT_INT_EQ(RDP_PAM_REFUSED, rdp_auth_pam(dir, "someone", "secret", "192.0.2.7"));
	return 0;
}

/* PAM's own delay after a failure is random (base 1.5 to 4.5 times here:
 * 3 to 9 s) and would tell a wrong password apart: it is off, the
 * refusal comes at once, and peer.c waits a fixed time itself. */
static int test_no_fail_delay(void)
{
	ASSERT_INT_EQ(0, service("auth required pam_faildelay.so delay=6000000\n"
				"auth required pam_deny.so\n"
				"account required pam_permit.so\n"));
	uint64_t start = now_us();
	ASSERT_INT_EQ(RDP_PAM_REFUSED, rdp_auth_pam(dir, "someone", "secret", "192.0.2.7"));
	ASSERT_TRUE(now_us() - start < 1000000);
	return 0;
}

/* The password is right, the account may not log in: told apart (peer.c
 * answers it after the same time as a wrong password). */
static int test_account(void)
{
	ASSERT_INT_EQ(0, service("auth required pam_permit.so\n"
				"account required pam_deny.so\n"));
	ASSERT_INT_EQ(RDP_PAM_ACCOUNT, rdp_auth_pam(dir, "someone", "secret", "192.0.2.7"));
	return 0;
}

/* wayrdp.pam as installed: pam_unix alone, nothing included from
 * common-auth, whose presence modules would count for a remote login. */
static int test_service_file(void)
{
	FILE* f = fopen(WAYRDP_PAM_FILE, "r");
	ASSERT_TRUE(f != NULL);
	char line[512];
	int auth = 0, account = 0, other = 0;
	while (fgets(line, sizeof(line), f)) {
		char type[64], control[64], module[256];
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%63s %63s %255s", type, control, module) != 3
				|| strcmp(module, "pam_unix.so") != 0)
			++other;
		else if (strcmp(type, "auth") == 0)
			++auth;
		else if (strcmp(type, "account") == 0)
			++account;
		else
			++other;
	}
	fclose(f);
	ASSERT_INT_EQ(1, auth);
	ASSERT_INT_EQ(1, account);
	ASSERT_INT_EQ(0, other);
	return 0;
}

static void cleanup(void)
{
	const char* names[] = { "wayrdp", "dump.sh", "dump" };
	char path[4096];
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		unlink(path);
	}
	rmdir(dir);
}

int main(void)
{
	if (!rdp_auth_have_pam()) {
		fprintf(stderr, "built without PAM\n");
		return 77;
	}
	if (!mkdtemp(dir))
		return 1;
	atexit(cleanup);
	char script[8192];
	snprintf(script, sizeof(script),
		"#!/bin/sh\n"
		"read -r password\n"
		"printf 'rhost=%%s\\nruser=%%s\\ntty=%%s\\nuser=%%s\\npassword=%%s\\n' "
		"\"$PAM_RHOST\" \"$PAM_RUSER\" \"$PAM_TTY\" \"$PAM_USER\" \"$password\" "
		">%s/dump\n", dir);
	if (write_file("dump.sh", script, 0755) < 0)
		return 1;

	int r = 0;
	RUN_TEST(test_items);
	RUN_TEST(test_refused);
	RUN_TEST(test_no_fail_delay);
	RUN_TEST(test_account);
	RUN_TEST(test_service_file);
	return r;
}
