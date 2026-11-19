// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal json-c pointer compatibility header (RFC 6901) for the PS3 port.
#ifndef CHIAKI_PS3_JSON_POINTER_H
#define CHIAKI_PS3_JSON_POINTER_H

#include "json_object.h"

#ifdef __cplusplus
extern "C" {
#endif

int json_pointer_get(struct json_object *obj, const char *path, struct json_object **res);

#ifdef __cplusplus
}
#endif

#endif
