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
 * Credentials and the TLS certificate: the SAM file NLA checks against, the
 * self-signed certificate made on first start, and the lockout after failed
 * logins.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct rdp_auth {
	/* Consecutive failed logins: a password that was checked and refused
	 * (see password_checked in internal.h), not a connection that went
	 * before one was checked. */
	int failures;
	uint64_t locked_until_us;
};

/* ~/.config/wayrdp, made with mode 0700 if it isn't there. NULL without a
 * home. Static storage. */
const char* rdp_config_dir(void);

/* Makes tls.crt and tls.key (mode 0600) if either is missing. */
int rdp_auth_ensure_certificate(const char* cert_file, const char* key_file);

/* Logs the certificate's SHA-1 thumbprint (what Windows shows) and SHA-256
 * fingerprint, to compare with the warning in mstsc. */
void rdp_auth_log_fingerprint(const char* cert_file);

/* The user wayrdp runs as. Static storage. */
const char* rdp_auth_user(void);

/* Writes "<user>:::<NT hash>:::" to sam_file with mode 0600. */
int rdp_auth_write_sam(const char* sam_file, const char* user,
		const char* password);

/* --set-password: asks twice without echo (or reads one line from a pipe)
 * and writes the SAM file. Returns the exit code. */
int rdp_auth_set_password(const char* sam_file);

/* Checks the password of user with PAM (service "wayrdp"), and that the
 * account may log in. Blocks: pam_faildelay alone takes seconds. False
 * without PAM support. */
bool rdp_auth_pam(const char* user, const char* password);
bool rdp_auth_have_pam(void);

/* Refuses a secret that others can read: -1, with the reason logged.
 * A missing file is fine (0). */
int rdp_auth_check_private(const char* path);

/* Lockout: three failures in a row lock new connections out for 30 s. */
bool rdp_auth_is_locked(const struct rdp_auth* self, uint64_t now_us);
void rdp_auth_failure(struct rdp_auth* self, uint64_t now_us);
void rdp_auth_success(struct rdp_auth* self);
