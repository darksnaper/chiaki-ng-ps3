// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal json-c compatibility header for the PS3 port (used by holepunch.c and psn.c).
#ifndef CHIAKI_PS3_JSON_OBJECT_H
#define CHIAKI_PS3_JSON_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int json_bool;

typedef enum json_type
{
	json_type_null,
	json_type_boolean,
	json_type_double,
	json_type_int,
	json_type_object,
	json_type_array,
	json_type_string
} json_type;

#define JSON_C_TO_STRING_PLAIN   0
#define JSON_C_TO_STRING_SPACED  (1 << 0)
#define JSON_C_TO_STRING_PRETTY  (1 << 1)

typedef struct json_object json_object;

int json_object_put(struct json_object *obj);
struct json_object *json_object_get(struct json_object *obj);

int json_object_is_type(const struct json_object *obj, enum json_type type);
enum json_type json_object_get_type(const struct json_object *obj);

json_bool json_object_object_get_ex(const struct json_object *obj, const char *key, struct json_object **value);
int json_object_object_length(const struct json_object *obj);

size_t json_object_array_length(const struct json_object *obj);
struct json_object *json_object_array_get_idx(const struct json_object *obj, size_t idx);

const char *json_object_get_string(struct json_object *obj);
int32_t json_object_get_int(const struct json_object *obj);
int64_t json_object_get_int64(const struct json_object *obj);
json_bool json_object_get_boolean(const struct json_object *obj);

const char *json_object_to_json_string_ext(struct json_object *obj, int flags);
const char *json_object_to_json_string(struct json_object *obj);

#ifdef __cplusplus
}
#endif

#endif
