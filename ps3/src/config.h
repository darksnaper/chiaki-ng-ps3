// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// chiaki.cfg: global settings plus one [host] section per console (PS4 or PS5).
#ifndef CHIAKI_PS3_CONFIG_H
#define CHIAKI_PS3_CONFIG_H

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_VERSION    "1.1.0-psn"
#define APP_DIR        "/dev_hdd0/game/CHIAKIPS3/USRDIR"
#define CFG_PATH       APP_DIR "/chiaki.cfg"
#define LOG_PATH       APP_DIR "/chiaki.log"
#define LOG_PREV_PATH  APP_DIR "/chiaki.prev.log"
#define STREAM_PATH    APP_DIR "/stream.h264"

#define CONFIG_HOSTS_MAX 8

typedef struct
{
	char name[32];
	char host[64];     // IP address; empty = find it on the network by host_id (or connect via PSN)
	char host_id[32];  // MAC address in hex as reported by discovery
	bool ps5;          // console type (PS4 if false)
	char regist_key[CHIAKI_SESSION_AUTH_SIZE];
	bool have_regist_key;
	uint8_t morning[0x10];
	bool have_morning;
	uint8_t duid[32];  // PSN Device UID (32 bytes / 64 hex chars) for remote PSN connection
	bool have_duid;
	bool use_psn;      // true = connect directly via PSN (Internet), skip LAN discovery
} ConfigHost;

typedef struct
{
	char path[128];    // file the settings came from / are saved to
	bool loaded;

	ConfigHost hosts[CONFIG_HOSTS_MAX];
	int host_count;

	uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	bool have_account_id;
	char psn_refresh_token[256];
	char psn_auth_token[256];
	int resolution;    // 360/540/720
	int fps;           // 30/60
	int bitrate;       // kbit/s, 0 = preset default
	int pacing;        // 0 = lowest delay, 1 = balanced, 2 = smoothest
	bool low_latency;  // SPS no-reorder rewrite + late frame pick
	bool vibration;    // DS3 rumble from the console
	bool port_guessing;// force STUN port guessing for symmetric / double NAT
	bool dump_stream;  // write the raw H.264 stream to STREAM_PATH
	bool verbose_log;
} Config;

static inline bool config_host_registered(const ConfigHost *h)
{
	return h->have_regist_key && h->have_morning;
}

static inline bool config_has_psn(const Config *cfg)
{
	return cfg->have_account_id && (cfg->psn_refresh_token[0] != '\0' || cfg->psn_auth_token[0] != '\0');
}

static inline bool config_host_connectable(const Config *cfg, const ConfigHost *h)
{
	return config_host_registered(h) || (config_has_psn(cfg) && (h->have_duid || h->use_psn));
}

// Defaults, then the HDD file, then the USB stick. Returns false if no file exists
// (cfg is still initialised with defaults and will be saved to the HDD).
bool config_load(Config *cfg);
// Finds chiaki.cfg on a USB stick (/dev_usb000 .. 007); writes its path to out.
bool config_find_usb(char *out, size_t out_size);
// Replaces everything in cfg with the file at path (NULL = first USB stick with
// a chiaki.cfg). cfg is untouched on failure; afterwards it saves to the hard drive.
bool config_import(Config *cfg, const char *path);
// Accepts the PSN account id as chiaki-ng's base64 text or as a decimal number.
bool config_parse_account_id(const char *text, uint8_t out[CHIAKI_PSN_ACCOUNT_ID_SIZE]);
// Base64 text of the account id, or "" if none is set.
void config_format_account_id(const Config *cfg, char *out, size_t out_size);
// Rewrites the whole file (to a temporary file first). Returns false on error.
bool config_save(const Config *cfg);

// Returns the new host or NULL if the list is full.
ConfigHost *config_add_host(Config *cfg);
void config_remove_host(Config *cfg, int index);
// Finds by host id (case-insensitive) or address; -1 if not found.
int config_find_host(const Config *cfg, const char *host_id, const char *addr);
// Finds by PSN DUID; -1 if not found.
int config_find_host_by_duid(const Config *cfg, const uint8_t duid[32]);
void config_host_set_registration(ConfigHost *h, const ChiakiRegisteredHost *reg);

#endif
