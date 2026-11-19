// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "config.h"

#include <chiaki/base64.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *s)
{
	while(isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while(e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static bool parse_hex(const char *hex, uint8_t *out, size_t out_size)
{
	if(strlen(hex) != out_size * 2)
		return false;
	for(size_t i = 0; i < out_size; i++)
	{
		unsigned int v;
		if(sscanf(hex + i * 2, "%2x", &v) != 1)
			return false;
		out[i] = (uint8_t)v;
	}
	return true;
}

static void hex_str(const uint8_t *buf, size_t size, char *out)
{
	for(size_t i = 0; i < size; i++)
		sprintf(out + i * 2, "%02x", buf[i]);
	out[size * 2] = '\0';
}

static bool parse_bool(const char *v)
{
	return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on");
}

static void set_defaults(Config *cfg, const char *path)
{
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->path, sizeof(cfg->path), "%s", path);
	cfg->resolution = 720;
	cfg->fps = 60;
	cfg->pacing = 1;
	cfg->low_latency = true;
	cfg->vibration = true;
}

static bool host_key(ConfigHost *h, const char *key, const char *val)
{
	if(!strcmp(key, "name"))
		snprintf(h->name, sizeof(h->name), "%s", val);
	else if(!strcmp(key, "host"))
	{
		// An unedited placeholder such as 192.168.1.XXX means "not set".
		if(!strstr(val, "XXX") && !strstr(val, "xxx"))
			snprintf(h->host, sizeof(h->host), "%s", val);
	}
	else if(!strcmp(key, "host_id") || !strcmp(key, "server_mac"))
		snprintf(h->host_id, sizeof(h->host_id), "%s", val);
	else if(!strcmp(key, "ps5"))
		h->ps5 = parse_bool(val);
	else if(!strcmp(key, "regist_key"))
	{
		// hex of the 16 byte key as chiaki stores it, or the plain text key
		memset(h->regist_key, 0, sizeof(h->regist_key));
		if(parse_hex(val, (uint8_t *)h->regist_key, sizeof(h->regist_key)))
			h->have_regist_key = true;
		else if(*val && strlen(val) <= sizeof(h->regist_key))
		{
			memcpy(h->regist_key, val, strlen(val));
			h->have_regist_key = true;
		}
	}
	else if(!strcmp(key, "rp_key") || !strcmp(key, "morning"))
		h->have_morning = parse_hex(val, h->morning, sizeof(h->morning));
	else if(!strcmp(key, "duid") || !strcmp(key, "psn_duid"))
		h->have_duid = parse_hex(val, h->duid, sizeof(h->duid));
	else if(!strcmp(key, "use_psn"))
		h->use_psn = parse_bool(val);
	else
		return false;
	return true;
}

static bool global_key(Config *cfg, const char *key, const char *val)
{
	if(!strcmp(key, "psn_account_id"))
	{
		size_t sz = sizeof(cfg->psn_account_id);
		cfg->have_account_id = chiaki_base64_decode(val, strlen(val), cfg->psn_account_id, &sz) == CHIAKI_ERR_SUCCESS
			&& sz == sizeof(cfg->psn_account_id);
	}
	else if(!strcmp(key, "psn_refresh_token"))
		snprintf(cfg->psn_refresh_token, sizeof(cfg->psn_refresh_token), "%s", val);
	else if(!strcmp(key, "psn_auth_token"))
		snprintf(cfg->psn_auth_token, sizeof(cfg->psn_auth_token), "%s", val);
	else if(!strcmp(key, "port_guessing"))
		cfg->port_guessing = parse_bool(val);
	else if(!strcmp(key, "resolution"))
		cfg->resolution = atoi(val);
	else if(!strcmp(key, "fps"))
		cfg->fps = atoi(val);
	else if(!strcmp(key, "bitrate"))
		cfg->bitrate = atoi(val);
	else if(!strcmp(key, "pacing"))
	{
		cfg->pacing = atoi(val);
		if(cfg->pacing < 0 || cfg->pacing > 2)
			cfg->pacing = 1;
	}
	else if(!strcmp(key, "low_latency"))
		cfg->low_latency = parse_bool(val);
	else if(!strcmp(key, "vibration"))
		cfg->vibration = parse_bool(val);
	else if(!strcmp(key, "dump_stream"))
		cfg->dump_stream = parse_bool(val);
	else if(!strcmp(key, "verbose_log"))
		cfg->verbose_log = parse_bool(val);
	else
		return false;
	return true;
}

static bool load_file(Config *cfg, const char *path)
{
	FILE *f = fopen(path, "r");
	if(!f)
		return false;
	set_defaults(cfg, path);
	cfg->loaded = true;

	ConfigHost *cur = NULL;     // inside a [host] section
	ConfigHost *legacy = NULL;  // host keys before any section (old single-console format)
	char line[512];
	while(fgets(line, sizeof(line), f))
	{
		char *l = trim(line);
		if(!*l || *l == '#' || *l == ';')
			continue;
		if(*l == '[')
		{
			cur = !strncasecmp(l, "[host]", 6) ? config_add_host(cfg) : NULL;
			continue;
		}
		char *eq = strchr(l, '=');
		if(!eq)
			continue;
		*eq = '\0';
		char *key = trim(l);
		char *val = trim(eq + 1);

		if(cur)
		{
			host_key(cur, key, val);
			continue;
		}
		if(global_key(cfg, key, val))
			continue;
		if(!legacy)
		{
			legacy = config_add_host(cfg);
			if(!legacy)
				continue;
		}
		host_key(legacy, key, val);
	}
	fclose(f);

	// Drop empty legacy entries (e.g. a file with only "host=" left blank and no keys).
	for(int i = cfg->host_count - 1; i >= 0; i--)
	{
		ConfigHost *h = &cfg->hosts[i];
		if(!h->host[0] && !h->host_id[0] && !h->have_regist_key && !h->have_duid && !h->name[0])
			config_remove_host(cfg, i);
	}
	return true;
}

bool config_load(Config *cfg)
{
	if(load_file(cfg, CFG_PATH))
		return true;
	if(config_import(cfg, NULL))
	{
		config_save(cfg); // keep a copy on the hard drive, so the stick is only needed once
		return true;
	}
	set_defaults(cfg, CFG_PATH);
	return false;
}

bool config_find_usb(char *out, size_t out_size)
{
	for(int i = 0; i < 8; i++)
	{
		snprintf(out, out_size, "/dev_usb%03d/chiaki.cfg", i);
		FILE *f = fopen(out, "r");
		if(f)
		{
			fclose(f);
			return true;
		}
	}
	out[0] = '\0';
	return false;
}

bool config_import(Config *cfg, const char *path)
{
	char usb[64];
	if(!path)
	{
		if(!config_find_usb(usb, sizeof(usb)))
			return false;
		path = usb;
	}
	static Config tmp; // too large for the stack of small threads
	if(!load_file(&tmp, path))
		return false;
	*cfg = tmp;
	// From now on the settings live on the hard drive.
	snprintf(cfg->path, sizeof(cfg->path), "%s", CFG_PATH);
	return true;
}

bool config_parse_account_id(const char *text, uint8_t out[CHIAKI_PSN_ACCOUNT_ID_SIZE])
{
	char t[40];
	size_t n = 0;
	bool digits = true;
	for(const char *c = text; *c; c++)
	{
		if(*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
			continue;
		if(n + 2 >= sizeof(t))
			return false;
		if(*c < '0' || *c > '9')
			digits = false;
		t[n++] = *c;
	}
	t[n] = '\0';
	if(!n)
		return false;

	if(digits)
	{
		// The account id as a number (up to 20 digits): stored as 8 bytes, little endian.
		if(n > 20)
			return false;
		uint64_t v = 0;
		for(size_t i = 0; i < n; i++)
		{
			uint64_t d = (uint64_t)(t[i] - '0');
			if(v > (UINT64_MAX - d) / 10)
				return false;
			v = v * 10 + d;
		}
		if(!v)
			return false;
		for(int i = 0; i < 8; i++)
			out[i] = (uint8_t)(v >> (8 * i));
		return true;
	}

	// Base64 as chiaki-ng shows it (11 characters and '='; the '=' may be left out).
	if(n == 11)
	{
		t[n++] = '=';
		t[n] = '\0';
	}
	uint8_t buf[16];
	size_t sz = sizeof(buf);
	if(chiaki_base64_decode(t, n, buf, &sz) != CHIAKI_ERR_SUCCESS || sz != CHIAKI_PSN_ACCOUNT_ID_SIZE)
		return false;
	bool zero = true;
	for(size_t i = 0; i < CHIAKI_PSN_ACCOUNT_ID_SIZE; i++)
		if(buf[i])
			zero = false;
	if(zero)
		return false; // the placeholder from chiaki.cfg.example
	memcpy(out, buf, CHIAKI_PSN_ACCOUNT_ID_SIZE);
	return true;
}

void config_format_account_id(const Config *cfg, char *out, size_t out_size)
{
	out[0] = '\0';
	if(cfg->have_account_id)
		chiaki_base64_encode(cfg->psn_account_id, sizeof(cfg->psn_account_id), out, out_size);
}

bool config_save(const Config *cfg)
{
	char tmp[144];
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->path);
	FILE *f = fopen(tmp, "w");
	if(!f)
		return false;

	fprintf(f, "# Chiaki PS3 configuration (rewritten by the app when settings change)\n\n");
	fprintf(f, "resolution=%d\nfps=%d\nbitrate=%d\n", cfg->resolution, cfg->fps, cfg->bitrate);
	fprintf(f, "# video pacing: 0 = lowest delay, 1 = balanced, 2 = smoothest\npacing=%d\n", cfg->pacing);
	fprintf(f, "low_latency=%d\nvibration=%d\n", cfg->low_latency ? 1 : 0, cfg->vibration ? 1 : 0);
	fprintf(f, "port_guessing=%d\n", cfg->port_guessing ? 1 : 0);
	fprintf(f, "dump_stream=%d\nverbose_log=%d\n", cfg->dump_stream ? 1 : 0, cfg->verbose_log ? 1 : 0);
	if(cfg->have_account_id)
	{
		char b64[32];
		if(chiaki_base64_encode(cfg->psn_account_id, sizeof(cfg->psn_account_id), b64, sizeof(b64)) == CHIAKI_ERR_SUCCESS)
			fprintf(f, "psn_account_id=%s\n", b64);
	}
	if(cfg->psn_refresh_token[0])
		fprintf(f, "psn_refresh_token=%s\n", cfg->psn_refresh_token);
	if(cfg->psn_auth_token[0])
		fprintf(f, "psn_auth_token=%s\n", cfg->psn_auth_token);

	for(int i = 0; i < cfg->host_count; i++)
	{
		const ConfigHost *h = &cfg->hosts[i];
		fprintf(f, "\n[host]\n");
		if(h->name[0])
			fprintf(f, "name=%s\n", h->name);
		fprintf(f, "host=%s\n", h->host);
		if(h->host_id[0])
			fprintf(f, "host_id=%s\n", h->host_id);
		fprintf(f, "ps5=%d\n", h->ps5 ? 1 : 0);
		if(h->use_psn)
			fprintf(f, "use_psn=1\n");
		if(h->have_duid)
		{
			char duid_hex[65];
			hex_str(h->duid, sizeof(h->duid), duid_hex);
			fprintf(f, "duid=%s\n", duid_hex);
		}
		if(config_host_registered(h))
		{
			char regist_hex[CHIAKI_SESSION_AUTH_SIZE * 2 + 1];
			char morning_hex[0x10 * 2 + 1];
			hex_str((const uint8_t *)h->regist_key, sizeof(h->regist_key), regist_hex);
			hex_str(h->morning, sizeof(h->morning), morning_hex);
			fprintf(f, "regist_key=%s\nrp_key=%s\n", regist_hex, morning_hex);
		}
	}

	bool ok = !ferror(f);
	if(fclose(f) != 0)
		ok = false;
	if(!ok)
	{
		remove(tmp);
		return false;
	}
	if(rename(tmp, cfg->path) == 0)
		return true;
	// Some file systems refuse to rename over an existing file.
	remove(cfg->path);
	return rename(tmp, cfg->path) == 0;
}

ConfigHost *config_add_host(Config *cfg)
{
	if(cfg->host_count >= CONFIG_HOSTS_MAX)
		return NULL;
	ConfigHost *h = &cfg->hosts[cfg->host_count++];
	memset(h, 0, sizeof(*h));
	return h;
}

void config_remove_host(Config *cfg, int index)
{
	if(index < 0 || index >= cfg->host_count)
		return;
	memmove(&cfg->hosts[index], &cfg->hosts[index + 1], (size_t)(cfg->host_count - index - 1) * sizeof(ConfigHost));
	cfg->host_count--;
}

int config_find_host(const Config *cfg, const char *host_id, const char *addr)
{
	for(int i = 0; i < cfg->host_count; i++)
		if(host_id && *host_id && cfg->hosts[i].host_id[0] && !strcasecmp(cfg->hosts[i].host_id, host_id))
			return i;
	for(int i = 0; i < cfg->host_count; i++)
		if(addr && *addr && !strcmp(cfg->hosts[i].host, addr))
			return i;
	return -1;
}

int config_find_host_by_duid(const Config *cfg, const uint8_t duid[32])
{
	if(!duid)
		return -1;
	for(int i = 0; i < cfg->host_count; i++)
		if(cfg->hosts[i].have_duid && memcmp(cfg->hosts[i].duid, duid, 32) == 0)
			return i;
	return -1;
}

void config_host_set_registration(ConfigHost *h, const ChiakiRegisteredHost *reg)
{
	memcpy(h->regist_key, reg->rp_regist_key, sizeof(h->regist_key));
	memcpy(h->morning, reg->rp_key, sizeof(h->morning));
	h->have_regist_key = h->have_morning = true;
	if(reg->server_nickname[0])
		snprintf(h->name, sizeof(h->name), "%s", reg->server_nickname);
	if(!h->host_id[0])
	{
		// server_mac -> host id format used by discovery (uppercase hex)
		bool nonzero = false;
		for(int i = 0; i < 6; i++)
			nonzero |= reg->server_mac[i] != 0;
		if(nonzero)
			snprintf(h->host_id, sizeof(h->host_id), "%02X%02X%02X%02X%02X%02X",
					reg->server_mac[0], reg->server_mac[1], reg->server_mac[2],
					reg->server_mac[3], reg->server_mac[4], reg->server_mac[5]);
	}
}
