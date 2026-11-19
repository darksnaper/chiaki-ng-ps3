// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Self-contained minimal json-c implementation (RFC 8259 + RFC 6901) for PS3.

#include <json-c/json_object.h>
#include <json-c/json_tokener.h>
#include <json-c/json_pointer.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct json_kv
{
	char *key;
	json_object *val;
} json_kv;

struct json_object
{
	json_type type;
	int refcount;
	char *serialized;
	union
	{
		bool b;
		int64_t i;
		double d;
		struct
		{
			char *ptr;
			size_t len;
		} s;
		struct
		{
			json_kv *entries;
			size_t count;
			size_t cap;
		} obj;
		struct
		{
			json_object **items;
			size_t count;
			size_t cap;
		} arr;
	} u;
};

typedef struct
{
	const char *p;
	const char *end;
	int depth;
} Parser;

typedef struct
{
	char *buf;
	size_t len;
	size_t cap;
} StrBuf;

static bool sb_reserve(StrBuf *sb, size_t extra)
{
	if(sb->len + extra + 1 <= sb->cap)
		return true;
	size_t ncap = sb->cap ? sb->cap * 2 : 64;
	while(ncap < sb->len + extra + 1)
		ncap *= 2;
	char *nbuf = realloc(sb->buf, ncap);
	if(!nbuf)
		return false;
	sb->buf = nbuf;
	sb->cap = ncap;
	return true;
}

static bool sb_append_n(StrBuf *sb, const char *s, size_t n)
{
	if(!sb_reserve(sb, n))
		return false;
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
	return true;
}

static bool sb_append_c(StrBuf *sb, char c)
{
	return sb_append_n(sb, &c, 1);
}

static bool sb_append_s(StrBuf *sb, const char *s)
{
	return sb_append_n(sb, s, strlen(s));
}

static json_object *json_new(json_type type)
{
	json_object *o = calloc(1, sizeof(json_object));
	if(!o)
		return NULL;
	o->type = type;
	o->refcount = 1;
	return o;
}

struct json_object *json_object_get(struct json_object *obj)
{
	if(obj)
		obj->refcount++;
	return obj;
}

int json_object_put(struct json_object *obj)
{
	if(!obj)
		return 0;
	obj->refcount--;
	if(obj->refcount > 0)
		return 0;

	free(obj->serialized);
	switch(obj->type)
	{
		case json_type_string:
			free(obj->u.s.ptr);
			break;
		case json_type_object:
			for(size_t i = 0; i < obj->u.obj.count; i++)
			{
				free(obj->u.obj.entries[i].key);
				json_object_put(obj->u.obj.entries[i].val);
			}
			free(obj->u.obj.entries);
			break;
		case json_type_array:
			for(size_t i = 0; i < obj->u.arr.count; i++)
				json_object_put(obj->u.arr.items[i]);
			free(obj->u.arr.items);
			break;
		default:
			break;
	}
	free(obj);
	return 1;
}

int json_object_is_type(const struct json_object *obj, enum json_type type)
{
	if(!obj)
		return type == json_type_null ? 1 : 0;
	return obj->type == type ? 1 : 0;
}

enum json_type json_object_get_type(const struct json_object *obj)
{
	return obj ? obj->type : json_type_null;
}

json_bool json_object_object_get_ex(const struct json_object *obj, const char *key, struct json_object **value)
{
	if(value)
		*value = NULL;
	if(!obj || obj->type != json_type_object || !key)
		return 0;
	for(size_t i = 0; i < obj->u.obj.count; i++)
	{
		if(strcmp(obj->u.obj.entries[i].key, key) == 0)
		{
			if(value)
				*value = obj->u.obj.entries[i].val;
			return 1;
		}
	}
	return 0;
}

int json_object_object_length(const struct json_object *obj)
{
	if(!obj || obj->type != json_type_object)
		return 0;
	return (int)obj->u.obj.count;
}

size_t json_object_array_length(const struct json_object *obj)
{
	if(!obj || obj->type != json_type_array)
		return 0;
	return obj->u.arr.count;
}

struct json_object *json_object_array_get_idx(const struct json_object *obj, size_t idx)
{
	if(!obj || obj->type != json_type_array || idx >= obj->u.arr.count)
		return NULL;
	return obj->u.arr.items[idx];
}

const char *json_object_get_string(struct json_object *obj)
{
	if(!obj)
		return NULL;
	if(obj->type == json_type_string)
		return obj->u.s.ptr ? obj->u.s.ptr : "";
	return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}

int64_t json_object_get_int64(const struct json_object *obj)
{
	if(!obj)
		return 0;
	switch(obj->type)
	{
		case json_type_int:
			return obj->u.i;
		case json_type_double:
			return (int64_t)obj->u.d;
		case json_type_boolean:
			return obj->u.b ? 1 : 0;
		case json_type_string:
			if(obj->u.s.ptr)
				return (int64_t)strtoll(obj->u.s.ptr, NULL, 10);
			return 0;
		default:
			return 0;
	}
}

int32_t json_object_get_int(const struct json_object *obj)
{
	int64_t v = json_object_get_int64(obj);
	if(v > INT32_MAX)
		return INT32_MAX;
	if(v < INT32_MIN)
		return INT32_MIN;
	return (int32_t)v;
}

json_bool json_object_get_boolean(const struct json_object *obj)
{
	if(!obj)
		return 0;
	switch(obj->type)
	{
		case json_type_boolean: return obj->u.b ? 1 : 0;
		case json_type_int:     return obj->u.i != 0 ? 1 : 0;
		case json_type_double:  return obj->u.d != 0.0 ? 1 : 0;
		case json_type_string:  return (obj->u.s.ptr && obj->u.s.len > 0) ? 1 : 0;
		default:                return 0;
	}
}

// ---------------------------------------------------------------------------
// JSON parser

static void skip_ws(Parser *ps)
{
	while(ps->p < ps->end)
	{
		unsigned char c = (unsigned char)*ps->p;
		if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
			ps->p++;
		else
			break;
	}
}

static int hex_val(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool parse_hex4(Parser *ps, uint32_t *out)
{
	if(ps->end - ps->p < 4)
		return false;
	uint32_t v = 0;
	for(int i = 0; i < 4; i++)
	{
		int h = hex_val(ps->p[i]);
		if(h < 0)
			return false;
		v = (v << 4) | (uint32_t)h;
	}
	ps->p += 4;
	*out = v;
	return true;
}

static bool sb_append_utf8(StrBuf *sb, uint32_t cp)
{
	char tmp[4];
	if(cp <= 0x7F)
	{
		tmp[0] = (char)cp;
		return sb_append_n(sb, tmp, 1);
	}
	if(cp <= 0x7FF)
	{
		tmp[0] = (char)(0xC0 | (cp >> 6));
		tmp[1] = (char)(0x80 | (cp & 0x3F));
		return sb_append_n(sb, tmp, 2);
	}
	if(cp <= 0xFFFF)
	{
		tmp[0] = (char)(0xE0 | (cp >> 12));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (char)(0x80 | (cp & 0x3F));
		return sb_append_n(sb, tmp, 3);
	}
	if(cp <= 0x10FFFF)
	{
		tmp[0] = (char)(0xF0 | (cp >> 18));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (char)(0x80 | (cp & 0x3F));
		return sb_append_n(sb, tmp, 4);
	}
	return false;
}

static bool parse_string_raw(Parser *ps, char **out_str, size_t *out_len)
{
	if(ps->p >= ps->end || *ps->p != '"')
		return false;
	ps->p++;

	StrBuf sb = { NULL, 0, 0 };
	if(!sb_reserve(&sb, 16))
		return false;
	sb.buf[0] = '\0';

	while(ps->p < ps->end)
	{
		unsigned char c = (unsigned char)*ps->p++;
		if(c == '"')
		{
			*out_str = sb.buf;
			if(out_len)
				*out_len = sb.len;
			return true;
		}
		if(c < 0x20)
		{
			free(sb.buf);
			return false;
		}
		if(c == '\\')
		{
			if(ps->p >= ps->end)
			{
				free(sb.buf);
				return false;
			}
			char esc = *ps->p++;
			switch(esc)
			{
				case '"':  if(!sb_append_c(&sb, '"'))  { free(sb.buf); return false; } break;
				case '\\': if(!sb_append_c(&sb, '\\')) { free(sb.buf); return false; } break;
				case '/':  if(!sb_append_c(&sb, '/'))  { free(sb.buf); return false; } break;
				case 'b':  if(!sb_append_c(&sb, '\b')) { free(sb.buf); return false; } break;
				case 'f':  if(!sb_append_c(&sb, '\f')) { free(sb.buf); return false; } break;
				case 'n':  if(!sb_append_c(&sb, '\n')) { free(sb.buf); return false; } break;
				case 'r':  if(!sb_append_c(&sb, '\r')) { free(sb.buf); return false; } break;
				case 't':  if(!sb_append_c(&sb, '\t')) { free(sb.buf); return false; } break;
				case 'u':
				{
					uint32_t cp = 0;
					if(!parse_hex4(ps, &cp))
					{
						free(sb.buf);
						return false;
					}
					if(cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u')
					{
						ps->p += 2;
						uint32_t low = 0;
						if(!parse_hex4(ps, &low) || low < 0xDC00 || low > 0xDFFF)
						{
							free(sb.buf);
							return false;
						}
						cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
					}
					if(!sb_append_utf8(&sb, cp))
					{
						free(sb.buf);
						return false;
					}
					break;
				}
				default:
					free(sb.buf);
					return false;
			}
		}
		else
		{
			if(!sb_append_c(&sb, (char)c))
			{
				free(sb.buf);
				return false;
			}
		}
	}

	free(sb.buf);
	return false;
}

static json_object *parse_value(Parser *ps);

static json_object *parse_object(Parser *ps)
{
	if(++ps->depth > JSON_MAX_DEPTH)
		return NULL;
	ps->p++; // skip '{'
	skip_ws(ps);

	json_object *obj = json_new(json_type_object);
	if(!obj)
		return NULL;

	if(ps->p < ps->end && *ps->p == '}')
	{
		ps->p++;
		ps->depth--;
		return obj;
	}

	while(ps->p < ps->end)
	{
		skip_ws(ps);
		char *key = NULL;
		if(!parse_string_raw(ps, &key, NULL))
			goto fail;
		skip_ws(ps);
		if(ps->p >= ps->end || *ps->p != ':')
		{
			free(key);
			goto fail;
		}
		ps->p++;
		json_object *val = parse_value(ps);
		if(!val)
		{
			free(key);
			goto fail;
		}

		// Replace existing key if duplicate
		bool replaced = false;
		for(size_t i = 0; i < obj->u.obj.count; i++)
		{
			if(strcmp(obj->u.obj.entries[i].key, key) == 0)
			{
				free(key);
				json_object_put(obj->u.obj.entries[i].val);
				obj->u.obj.entries[i].val = val;
				replaced = true;
				break;
			}
		}
		if(!replaced)
		{
			if(obj->u.obj.count == obj->u.obj.cap)
			{
				size_t ncap = obj->u.obj.cap ? obj->u.obj.cap * 2 : 8;
				json_kv *ne = realloc(obj->u.obj.entries, ncap * sizeof(json_kv));
				if(!ne)
				{
					free(key);
					json_object_put(val);
					goto fail;
				}
				obj->u.obj.entries = ne;
				obj->u.obj.cap = ncap;
			}
			obj->u.obj.entries[obj->u.obj.count].key = key;
			obj->u.obj.entries[obj->u.obj.count].val = val;
			obj->u.obj.count++;
		}

		skip_ws(ps);
		if(ps->p < ps->end && *ps->p == ',')
		{
			ps->p++;
			continue;
		}
		if(ps->p < ps->end && *ps->p == '}')
		{
			ps->p++;
			ps->depth--;
			return obj;
		}
		goto fail;
	}

fail:
	ps->depth--;
	json_object_put(obj);
	return NULL;
}

static json_object *parse_array(Parser *ps)
{
	if(++ps->depth > JSON_MAX_DEPTH)
		return NULL;
	ps->p++; // skip '['
	skip_ws(ps);

	json_object *arr = json_new(json_type_array);
	if(!arr)
		return NULL;

	if(ps->p < ps->end && *ps->p == ']')
	{
		ps->p++;
		ps->depth--;
		return arr;
	}

	while(ps->p < ps->end)
	{
		json_object *item = parse_value(ps);
		if(!item)
			goto fail;

		if(arr->u.arr.count == arr->u.arr.cap)
		{
			size_t ncap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 8;
			json_object **ni = realloc(arr->u.arr.items, ncap * sizeof(json_object *));
			if(!ni)
			{
				json_object_put(item);
				goto fail;
			}
			arr->u.arr.items = ni;
			arr->u.arr.cap = ncap;
		}
		arr->u.arr.items[arr->u.arr.count++] = item;

		skip_ws(ps);
		if(ps->p < ps->end && *ps->p == ',')
		{
			ps->p++;
			continue;
		}
		if(ps->p < ps->end && *ps->p == ']')
		{
			ps->p++;
			ps->depth--;
			return arr;
		}
		goto fail;
	}

fail:
	ps->depth--;
	json_object_put(arr);
	return NULL;
}

static json_object *parse_number(Parser *ps)
{
	const char *start = ps->p;
	if(ps->p < ps->end && *ps->p == '-')
		ps->p++;
	if(ps->p >= ps->end || !isdigit((unsigned char)*ps->p))
		return NULL;
	if(*ps->p == '0')
		ps->p++;
	else
	{
		while(ps->p < ps->end && isdigit((unsigned char)*ps->p))
			ps->p++;
	}

	bool is_float = false;
	if(ps->p < ps->end && *ps->p == '.')
	{
		is_float = true;
		ps->p++;
		if(ps->p >= ps->end || !isdigit((unsigned char)*ps->p))
			return NULL;
		while(ps->p < ps->end && isdigit((unsigned char)*ps->p))
			ps->p++;
	}
	if(ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
	{
		is_float = true;
		ps->p++;
		if(ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
			ps->p++;
		if(ps->p >= ps->end || !isdigit((unsigned char)*ps->p))
			return NULL;
		while(ps->p < ps->end && isdigit((unsigned char)*ps->p))
			ps->p++;
	}

	size_t len = (size_t)(ps->p - start);
	char numbuf[128];
	if(len >= sizeof(numbuf))
		return NULL;
	memcpy(numbuf, start, len);
	numbuf[len] = '\0';

	if(is_float)
	{
		json_object *o = json_new(json_type_double);
		if(!o)
			return NULL;
		o->u.d = strtod(numbuf, NULL);
		return o;
	}
	else
	{
		json_object *o = json_new(json_type_int);
		if(!o)
			return NULL;
		errno = 0;
		o->u.i = (int64_t)strtoll(numbuf, NULL, 10);
		return o;
	}
}

static json_object *parse_value(Parser *ps)
{
	skip_ws(ps);
	if(ps->p >= ps->end)
		return NULL;

	char c = *ps->p;
	if(c == '{')
		return parse_object(ps);
	if(c == '[')
		return parse_array(ps);
	if(c == '"')
	{
		char *s = NULL;
		size_t slen = 0;
		if(!parse_string_raw(ps, &s, &slen))
			return NULL;
		json_object *o = json_new(json_type_string);
		if(!o)
		{
			free(s);
			return NULL;
		}
		o->u.s.ptr = s;
		o->u.s.len = slen;
		return o;
	}
	if(c == '-' || isdigit((unsigned char)c))
		return parse_number(ps);
	if(ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0)
	{
		ps->p += 4;
		json_object *o = json_new(json_type_boolean);
		if(o)
			o->u.b = true;
		return o;
	}
	if(ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0)
	{
		ps->p += 5;
		json_object *o = json_new(json_type_boolean);
		if(o)
			o->u.b = false;
		return o;
	}
	if(ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0)
	{
		ps->p += 4;
		return json_new(json_type_null);
	}
	return NULL;
}

struct json_tokener *json_tokener_new(void)
{
	return calloc(1, sizeof(json_tokener));
}

void json_tokener_free(struct json_tokener *tok)
{
	free(tok);
}

struct json_object *json_tokener_parse_ex(struct json_tokener *tok, const char *str, int len)
{
	if(!str)
		return NULL;
	size_t n = (len < 0) ? strlen(str) : (size_t)len;
	// Trim any trailing NUL inside len
	for(size_t i = 0; i < n; i++)
	{
		if(str[i] == '\0')
		{
			n = i;
			break;
		}
	}
	Parser ps = { str, str + n, 0 };
	json_object *res = parse_value(&ps);
	if(tok)
		tok->err = res ? 0 : -1;
	return res;
}

struct json_object *json_tokener_parse(const char *str)
{
	if(!str)
		return NULL;
	return json_tokener_parse_ex(NULL, str, (int)strlen(str));
}

// ---------------------------------------------------------------------------
// JSON serializer

static bool serialize_string(StrBuf *sb, const char *s, size_t len)
{
	if(!sb_append_c(sb, '"'))
		return false;
	for(size_t i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)s[i];
		switch(c)
		{
			case '"':  if(!sb_append_s(sb, "\\\"")) return false; break;
			case '\\': if(!sb_append_s(sb, "\\\\")) return false; break;
			case '\b': if(!sb_append_s(sb, "\\b"))  return false; break;
			case '\f': if(!sb_append_s(sb, "\\f"))  return false; break;
			case '\n': if(!sb_append_s(sb, "\\n"))  return false; break;
			case '\r': if(!sb_append_s(sb, "\\r"))  return false; break;
			case '\t': if(!sb_append_s(sb, "\\t"))  return false; break;
			default:
				if(c < 0x20)
				{
					char ubuf[8];
					snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned)c);
					if(!sb_append_s(sb, ubuf))
						return false;
				}
				else
				{
					if(!sb_append_c(sb, (char)c))
						return false;
				}
				break;
		}
	}
	return sb_append_c(sb, '"');
}

static bool serialize_value(StrBuf *sb, const json_object *obj, int depth)
{
	if(depth > JSON_MAX_DEPTH)
		return sb_append_s(sb, "null");
	if(!obj)
		return sb_append_s(sb, "null");

	char numbuf[64];
	switch(obj->type)
	{
		case json_type_null:
			return sb_append_s(sb, "null");
		case json_type_boolean:
			return sb_append_s(sb, obj->u.b ? "true" : "false");
		case json_type_int:
			snprintf(numbuf, sizeof(numbuf), "%lld", (long long)obj->u.i);
			return sb_append_s(sb, numbuf);
		case json_type_double:
			snprintf(numbuf, sizeof(numbuf), "%.15g", obj->u.d);
			return sb_append_s(sb, numbuf);
		case json_type_string:
			return serialize_string(sb, obj->u.s.ptr ? obj->u.s.ptr : "", obj->u.s.len);
		case json_type_array:
			if(!sb_append_c(sb, '['))
				return false;
			for(size_t i = 0; i < obj->u.arr.count; i++)
			{
				if(i > 0 && !sb_append_c(sb, ','))
					return false;
				if(!serialize_value(sb, obj->u.arr.items[i], depth + 1))
					return false;
			}
			return sb_append_c(sb, ']');
		case json_type_object:
			if(!sb_append_c(sb, '{'))
				return false;
			for(size_t i = 0; i < obj->u.obj.count; i++)
			{
				if(i > 0 && !sb_append_c(sb, ','))
					return false;
				if(!serialize_string(sb, obj->u.obj.entries[i].key, strlen(obj->u.obj.entries[i].key)))
					return false;
				if(!sb_append_c(sb, ':'))
					return false;
				if(!serialize_value(sb, obj->u.obj.entries[i].val, depth + 1))
					return false;
			}
			return sb_append_c(sb, '}');
	}
	return false;
}

const char *json_object_to_json_string_ext(struct json_object *obj, int flags)
{
	(void)flags;
	if(!obj)
		return "null";
	StrBuf sb = { NULL, 0, 0 };
	if(!serialize_value(&sb, obj, 0))
	{
		free(sb.buf);
		return "";
	}
	free(obj->serialized);
	obj->serialized = sb.buf;
	return obj->serialized;
}

const char *json_object_to_json_string(struct json_object *obj)
{
	return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}

// ---------------------------------------------------------------------------
// RFC 6901 JSON Pointer

int json_pointer_get(struct json_object *obj, const char *path, struct json_object **res)
{
	if(res)
		*res = NULL;
	if(!obj || !path)
		return -1;
	if(path[0] == '\0')
	{
		if(res)
			*res = obj;
		return 0;
	}
	if(path[0] != '/')
		return -1;

	struct json_object *cur = obj;
	const char *p = path;
	while(*p == '/')
	{
		p++;
		const char *slash = strchr(p, '/');
		size_t raw_len = slash ? (size_t)(slash - p) : strlen(p);

		// Unescape ~1 -> / and ~0 -> ~
		char keybuf[256];
		if(raw_len >= sizeof(keybuf))
			return -1;
		size_t klen = 0;
		for(size_t i = 0; i < raw_len; i++)
		{
			if(p[i] == '~' && i + 1 < raw_len)
			{
				if(p[i + 1] == '1')
				{
					keybuf[klen++] = '/';
					i++;
					continue;
				}
				if(p[i + 1] == '0')
				{
					keybuf[klen++] = '~';
					i++;
					continue;
				}
			}
			keybuf[klen++] = p[i];
		}
		keybuf[klen] = '\0';

		if(cur->type == json_type_object)
		{
			struct json_object *next = NULL;
			if(!json_object_object_get_ex(cur, keybuf, &next))
				return -1;
			cur = next;
		}
		else if(cur->type == json_type_array)
		{
			if(klen == 0)
				return -1;
			for(size_t i = 0; i < klen; i++)
				if(!isdigit((unsigned char)keybuf[i]))
					return -1;
			unsigned long idx = strtoul(keybuf, NULL, 10);
			if(idx >= cur->u.arr.count)
				return -1;
			cur = cur->u.arr.items[idx];
		}
		else
		{
			return -1;
		}

		p = slash ? slash : p + raw_len;
	}

	if(res)
		*res = cur;
	return 0;
}
