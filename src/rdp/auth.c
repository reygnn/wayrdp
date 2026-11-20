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

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <winpr/ntlm.h>
#include <winpr/ssl.h>
#include <winpr/tools/makecert.h>
#include <freerdp/crypto/certificate.h>

#include "rdp/auth.h"
#include "config.h"
#include "log.h"

#define MAX_FAILURES 3
#define LOCKOUT_US (30 * 1000000ULL)

const char* rdp_config_dir(void)
{
	static char path[4096];
	if (path[0])
		return path;

	const char* xdg = getenv("XDG_CONFIG_HOME");
	const char* home = getenv("HOME");
	if (xdg && xdg[0])
		snprintf(path, sizeof(path), "%s/wayrdp", xdg);
	else if (home && home[0])
		snprintf(path, sizeof(path), "%s/.config/wayrdp", home);
	else
		return NULL;

	/* ~/.config may be missing on a fresh account. */
	char parent[4096];
	snprintf(parent, sizeof(parent), "%s", path);
	char* slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		mkdir(parent, 0700);
	}
	if (mkdir(path, 0700) < 0 && errno != EEXIST) {
		wv_log(WV_LOG_ERROR, "Can't create %s: %m", path);
		path[0] = '\0';
		return NULL;
	}
	return path;
}

static bool file_exists(const char* path)
{
	return access(path, F_OK) == 0;
}

/* makecert writes <dir>/<name>.crt and <dir>/<name>.key; wayrdp wants the
 * paths from the config, so it makes them in a temporary directory and moves
 * them. */
int rdp_auth_ensure_certificate(const char* cert_file, const char* key_file)
{
	if (file_exists(cert_file) && file_exists(key_file))
		return 0;

	wv_log(WV_LOG_INFO, "Creating a self-signed certificate: %s", cert_file);

	char tmpdir[] = "/tmp/wayrdp-cert.XXXXXX";
	if (!mkdtemp(tmpdir)) {
		wv_log(WV_LOG_ERROR, "mkdtemp: %m");
		return -1;
	}

	int rc = -1;
	char crt[sizeof(tmpdir) + 16], key[sizeof(tmpdir) + 16];
	snprintf(crt, sizeof(crt), "%s/tls.crt", tmpdir);
	snprintf(key, sizeof(key), "%s/tls.key", tmpdir);
	mode_t old_umask = umask(077);

	char* argv[] = { "makecert", "-rdp", "-live", "-silent", "-y", "10",
		"-n", "CN=wayrdp" };
	MAKECERT_CONTEXT* mc = makecert_context_new();
	if (!mc)
		goto out;
	if (makecert_context_process(mc, sizeof(argv) / sizeof(argv[0]), argv) < 0
			|| makecert_context_set_output_file_name(mc, "tls") != 1
			|| makecert_context_output_certificate_file(mc, tmpdir) != 1
			|| makecert_context_output_private_key_file(mc, tmpdir) != 1) {
		wv_log(WV_LOG_ERROR, "makecert failed");
		goto out;
	}

	/* /tmp and the config dir may be on different file systems. */
	const char* from[] = { crt, key };
	const char* to[] = { cert_file, key_file };
	for (int i = 0; i < 2; ++i) {
		FILE* in = fopen(from[i], "r");
		int fd = open(to[i], O_WRONLY | O_CREAT | O_TRUNC, 0600);
		FILE* out = fd >= 0 ? fdopen(fd, "w") : NULL;
		if (!in || !out) {
			wv_log(WV_LOG_ERROR, "Can't write %s: %m", to[i]);
			if (in) fclose(in);
			if (out) fclose(out); else if (fd >= 0) close(fd);
			goto out;
		}
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
			fwrite(buf, 1, n, out);
		fclose(in);
		if (fclose(out) != 0) {
			wv_log(WV_LOG_ERROR, "Can't write %s: %m", to[i]);
			goto out;
		}
	}
	rc = 0;

out:
	umask(old_umask);
	makecert_context_free(mc);
	unlink(crt);
	unlink(key);
	rmdir(tmpdir);
	return rc;
}

static void log_hash(const rdpCertificate* cert, const char* hash,
		const char* what)
{
	char* fp = freerdp_certificate_get_fingerprint_by_hash(cert, hash);
	if (fp) {
		wv_log(WV_LOG_WARNING, "Certificate %s: %s", what, fp);
		free(fp);
	}
}

void rdp_auth_log_fingerprint(const char* cert_file)
{
	rdpCertificate* cert = freerdp_certificate_new_from_file(cert_file);
	if (!cert)
		return;
	/* Windows' certificate dialog shows the SHA-1 "thumbprint". */
	log_hash(cert, "sha1", "SHA-1 thumbprint");
	log_hash(cert, "sha256", "SHA-256");
	freerdp_certificate_free(cert);
}

const char* rdp_auth_user(void)
{
	static char name[256];
	if (name[0])
		return name;
	struct passwd* pw = getpwuid(getuid());
	if (!pw)
		return NULL;
	snprintf(name, sizeof(name), "%s", pw->pw_name);
	return name;
}

int rdp_auth_write_sam(const char* sam_file, const char* user,
		const char* password)
{
	BYTE hash[16];
	/* OpenSSL 3 has MD4 only in its legacy provider, which this loads. */
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
	if (!NTOWFv1A((LPSTR)password, strlen(password), hash)) {
		wv_log(WV_LOG_ERROR, "Can't compute the NT hash");
		return -1;
	}

	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.new", sam_file);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		wv_log(WV_LOG_ERROR, "Can't write %s: %m", tmp);
		return -1;
	}
	fchmod(fd, 0600);

	char line[512];
	int len = snprintf(line, sizeof(line), "%s:::", user);
	for (int i = 0; i < 16; ++i)
		len += snprintf(line + len, sizeof(line) - len, "%02x", hash[i]);
	len += snprintf(line + len, sizeof(line) - len, ":::\n");

	bool ok = write(fd, line, len) == len;
	ok = fsync(fd) == 0 && ok;
	close(fd);
	memset(hash, 0, sizeof(hash));
	memset(line, 0, sizeof(line));

	if (!ok || rename(tmp, sam_file) < 0) {
		wv_log(WV_LOG_ERROR, "Can't write %s: %m", sam_file);
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* One line without the newline, without echo on a terminal. -1 without
 * one, -2 (said) for one too long for buf. */
static int read_secret(const char* prompt, char* buf, size_t size)
{
	bool tty = isatty(STDIN_FILENO);
	struct termios old, noecho;
	if (tty) {
		fprintf(stderr, "%s", prompt);
		fflush(stderr);
		tcgetattr(STDIN_FILENO, &old);
		noecho = old;
		noecho.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
	}
	char* line = fgets(buf, size, stdin);
	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fprintf(stderr, "\n");
	}
	if (!line)
		return -1;
	size_t n = strcspn(buf, "\n");
	if (buf[n] != '\n' && !feof(stdin)) {
		/* Longer than buf: refused rather than cut short, the rest of
		 * the line read away. */
		int c;
		while ((c = getchar()) != EOF && c != '\n')
			;
		memset(buf, 0, size);
		fprintf(stderr, "The password is too long (at most %zu bytes)\n",
				size - 2);
		return -2;
	}
	buf[n] = '\0';
	return 0;
}

int rdp_auth_set_password(const char* sam_file)
{
	const char* user = rdp_auth_user();
	if (!user) {
		wv_log(WV_LOG_ERROR, "Who am I?");
		return 1;
	}

	char pw1[256], pw2[256];
	int got = read_secret("RDP password: ", pw1, sizeof(pw1));
	if (got < 0) {
		if (got == -1)
			wv_log(WV_LOG_ERROR, "No password given");
		return 1;
	}
	if (isatty(STDIN_FILENO)) {
		if (read_secret("Again: ", pw2, sizeof(pw2)) < 0
				|| strcmp(pw1, pw2) != 0) {
			memset(pw1, 0, sizeof(pw1));
			memset(pw2, 0, sizeof(pw2));
			fprintf(stderr, "The passwords differ.\n");
			return 1;
		}
	}
	memset(pw2, 0, sizeof(pw2));

	if (pw1[0] == '\0') {
		fprintf(stderr, "An empty password is not allowed.\n");
		return 1;
	}

	int rc = rdp_auth_write_sam(sam_file, user, pw1);
	memset(pw1, 0, sizeof(pw1));
	if (rc < 0)
		return 1;

	fprintf(stderr, "Password for %s written to %s\n", user, sam_file);
	return 0;
}

int rdp_auth_check_private(const char* path)
{
	struct stat st;
	if (stat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	if (st.st_uid != getuid()) {
		wv_log(WV_LOG_ERROR, "%s belongs to someone else", path);
		return -1;
	}
	if (st.st_mode & 077) {
		wv_log(WV_LOG_ERROR, "%s can be read by others (mode %03o); chmod 600 it",
				path, st.st_mode & 0777);
		return -1;
	}
	return 0;
}

bool rdp_auth_is_locked(const struct rdp_auth* self, uint64_t now_us)
{
	return now_us < self->locked_until_us;
}

void rdp_auth_failure(struct rdp_auth* self, uint64_t now_us)
{
	if (++self->failures < MAX_FAILURES)
		return;
	self->failures = 0;
	self->locked_until_us = now_us + LOCKOUT_US;
	wv_log(WV_LOG_WARNING, "%d failed logins: no new connections for %llu s",
			MAX_FAILURES, LOCKOUT_US / 1000000ULL);
}

void rdp_auth_success(struct rdp_auth* self)
{
	self->failures = 0;
}
