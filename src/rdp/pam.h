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
 * -a pam: the Linux password, checked with PAM (service "wayrdp").
 *
 * Only libpam, no FreeRDP: test/unit/pam-test.c runs it against a PAM
 * configuration of its own (pam_start_confdir).
 */

#pragma once

#include <stdbool.h>

enum rdp_pam_result {
	RDP_PAM_OK,
	/* The password is wrong. */
	RDP_PAM_REFUSED,
	/* The password is right, but the account may not log in (expired,
	 * locked, a new password wanted). */
	RDP_PAM_ACCOUNT,
	/* PAM itself failed, or there is no PAM. */
	RDP_PAM_ERROR,
};

/* Every refusal is answered this long after the check began, whatever
 * was wrong, so its time tells nothing. The caller waits (peer.c, on the
 * main loop, not in the worker); rdp_auth_pam doesn't. */
#define RDP_PAM_REFUSAL_US 2000000

/* Checks user's password and that the account may log in. confdir is
 * where the service's file is, NULL for /etc/pam.d. rhost, the client's
 * address, goes to PAM as PAM_RHOST, so modules and logs see a login
 * from elsewhere. PAM's own delay after a failure is off: it is random
 * (pam_unix: 1 to 3 s) and would tell a wrong password from the rest. */
enum rdp_pam_result rdp_auth_pam(const char* confdir, const char* user,
		const char* password, const char* rhost);
bool rdp_auth_have_pam(void);
