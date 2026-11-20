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


#include <stdlib.h>
#include <string.h>

#include "rdp/pam.h"
#include "config.h"
#include "log.h"

#ifdef ENABLE_PAM
#include <security/pam_appl.h>

/* Instead of PAM's sleep after a failure: nothing, wayrdp waits itself. */
static void no_fail_delay(int retval, unsigned usec_delay, void* appdata)
{
}

static int pam_conversation(int n, const struct pam_message** msgs,
		struct pam_response** responses, void* data)
{
	const char* password = data;
	struct pam_response* r = calloc(n, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;

	for (int i = 0; i < n; ++i) {
		switch (msgs[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			/* Only one thing to say: the password. */
			r[i].resp = strdup(password);
			if (!r[i].resp)
				goto failure;
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			break;
		default:
			goto failure;
		}
	}
	*responses = r;
	return PAM_SUCCESS;

failure:
	for (int i = 0; i < n; ++i) {
		if (r[i].resp) {
			memset(r[i].resp, 0, strlen(r[i].resp));
			free(r[i].resp);
		}
	}
	free(r);
	return PAM_CONV_ERR;
}

enum rdp_pam_result rdp_auth_pam(const char* confdir, const char* user,
		const char* password, const char* rhost)
{
	struct pam_conv conv = {
		.conv = pam_conversation,
		.appdata_ptr = (void*)password,
	};
	pam_handle_t* pam = NULL;
	int rc = pam_start_confdir("wayrdp", user, &conv, confdir, &pam);
	if (rc != PAM_SUCCESS) {
		wv_log(WV_LOG_ERROR, "PAM: %s", pam_strerror(pam, rc));
		return RDP_PAM_ERROR;
	}

	/* A login from elsewhere: modules that let the user in by presence at
	 * the machine (a fingerprint) go by PAM_RHOST, and logs show it. */
	rc = pam_set_item(pam, PAM_FAIL_DELAY, (const void*)no_fail_delay);
	if (rc == PAM_SUCCESS && rhost && rhost[0])
		rc = pam_set_item(pam, PAM_RHOST, rhost);
	if (rc == PAM_SUCCESS)
		rc = pam_set_item(pam, PAM_RUSER, user);
	if (rc == PAM_SUCCESS)
		rc = pam_set_item(pam, PAM_TTY, "wayrdp");

	enum rdp_pam_result result = RDP_PAM_ERROR;
	if (rc == PAM_SUCCESS) {
		rc = pam_authenticate(pam, PAM_DISALLOW_NULL_AUTHTOK);
		result = RDP_PAM_REFUSED;
	}
	if (rc == PAM_SUCCESS) {
		rc = pam_acct_mgmt(pam, PAM_DISALLOW_NULL_AUTHTOK);
		result = RDP_PAM_ACCOUNT;
	}
	if (rc == PAM_SUCCESS)
		result = RDP_PAM_OK;
	else
		wv_log(WV_LOG_WARNING, "PAM: %s for %s", pam_strerror(pam, rc), user);

	pam_end(pam, rc);
	return result;
}

bool rdp_auth_have_pam(void)
{
	return true;
}
#else
enum rdp_pam_result rdp_auth_pam(const char* confdir, const char* user,
		const char* password, const char* rhost)
{
	return RDP_PAM_ERROR;
}

bool rdp_auth_have_pam(void)
{
	return false;
}
#endif
