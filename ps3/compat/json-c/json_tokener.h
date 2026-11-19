// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal json-c tokener compatibility header for the PS3 port.
#ifndef CHIAKI_PS3_JSON_TOKENER_H
#define CHIAKI_PS3_JSON_TOKENER_H

#include "json_object.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json_tokener
{
	int err;
} json_tokener;

struct json_tokener *json_tokener_new(void);
void json_tokener_free(struct json_tokener *tok);
struct json_object *json_tokener_parse(const char *str);
struct json_object *json_tokener_parse_ex(struct json_tokener *tok, const char *str, int len);

#ifdef __cplusplus
}
#endif

#endif
