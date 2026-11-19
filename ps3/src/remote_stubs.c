// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 port: AIA certificate-chain recovery stubs.
// PSN hole punching itself is now compiled natively from lib/src/remote/holepunch.c
// on top of ps3_curl.c (mbedTLS) and ps3_json.c.

#include <chiaki/common.h>
#include <chiaki/time.h>
#include "aia.h"

ChiakiErrorCode chiaki_aia_init(void) { return CHIAKI_ERR_SUCCESS; }
void chiaki_aia_fini(void) {}

bool chiaki_aia_should_stop(const ChiakiAiaControl *control)
{
	return control && ((control->canceled && control->canceled(control->user)) ||
		(control->deadline_ms && chiaki_time_now_monotonic_ms() >= control->deadline_ms));
}

size_t chiaki_aia_blob_len(void) { return 0; }
uint32_t chiaki_aia_blob_generation(void) { return 0; }
void chiaki_aia_blob_reset(void) {}

bool chiaki_aia_blob_add_pem(const char *pem, size_t pem_len)
{
	(void)pem; (void)pem_len;
	return false;
}

char *chiaki_aia_blob_take(size_t *len_out)
{
	if(len_out)
		*len_out = 0;
	return NULL;
}

char *chiaki_aia_blob_with_ca(FILE *ca_file, size_t *len_out)
{
	(void)ca_file;
	if(len_out)
		*len_out = 0;
	return NULL;
}

bool chiaki_aia_peek_leaf(const char *host, uint8_t **der_out, size_t *der_len_out,
	ChiakiLog *log, const ChiakiAiaControl *control)
{
	(void)host; (void)log; (void)control;
	if(der_out) *der_out = NULL;
	if(der_len_out) *der_len_out = 0;
	return false;
}

ChiakiErrorCode chiaki_aia_recover(const uint8_t *leaf_der, size_t leaf_len,
	char **pem_out, size_t *pem_len_out, ChiakiLog *log, const ChiakiAiaControl *control)
{
	(void)leaf_der; (void)leaf_len; (void)log; (void)control;
	if(pem_out) *pem_out = NULL;
	if(pem_len_out) *pem_len_out = 0;
	return CHIAKI_ERR_UNKNOWN;
}
