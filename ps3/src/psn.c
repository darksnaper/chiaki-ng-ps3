// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PSN OAuth2 token refresh, cloud console discovery, and NAT hole punching for PS3.

#include "psn.h"
#include "app.h"

#include <chiaki/base64.h>
#include <chiaki/thread.h>
#include <curl/curl.h>
#include <json-c/json_object.h>
#include <json-c/json_tokener.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PSN_CLIENT_ID     "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PSN_TOKEN_URL     "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
#define PSN_OAUTH_SCOPE   "psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update"
#define PSN_REDIRECT_URI  "https://remoteplay.dl.playstation.net/remoteplay/redirect"

typedef struct
{
	char *data;
	size_t size;
} HttpBuf;

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *user)
{
	size_t n = size * nmemb;
	HttpBuf *b = user;
	char *tmp = realloc(b->data, b->size + n + 1);
	if(!tmp)
		return 0;
	b->data = tmp;
	memcpy(b->data + b->size, ptr, n);
	b->size += n;
	b->data[b->size] = '\0';
	return n;
}

static void set_err(char *buf, size_t size, const char *fmt, ...)
{
	if(!buf || !size)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, size, fmt, ap);
	va_end(ap);
}

bool psn_refresh_token(Config *cfg, char *err_buf, size_t err_size)
{
	if(!cfg->psn_refresh_token[0])
	{
		set_err(err_buf, err_size, "No PSN refresh token configured.");
		return false;
	}

	const char *cred = PSN_CLIENT_ID ":" PSN_CLIENT_SECRET;
	char cred_b64[128] = {0};
	if(chiaki_base64_encode((const uint8_t *)cred, strlen(cred), cred_b64, sizeof(cred_b64)) != CHIAKI_ERR_SUCCESS)
	{
		set_err(err_buf, err_size, "Failed to encode PSN OAuth credentials.");
		return false;
	}

	char auth_hdr[192];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s", cred_b64);

	char body[768];
	snprintf(body, sizeof(body),
		"grant_type=refresh_token&refresh_token=%s&scope=%s&redirect_uri=%s&",
		cfg->psn_refresh_token, PSN_OAUTH_SCOPE, PSN_REDIRECT_URI);

	CURL *curl = curl_easy_init();
	if(!curl)
	{
		set_err(err_buf, err_size, "Out of memory initializing HTTP client.");
		return false;
	}

	HttpBuf resp = { NULL, 0 };
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(curl, CURLOPT_URL, PSN_TOKEN_URL);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

	app_log("PSN: refreshing OAuth2 access token");
	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if(res != CURLE_OK)
	{
		if(res == CURLE_HTTP_RETURNED_ERROR)
			set_err(err_buf, err_size, "PSN token refresh failed (HTTP %ld). Re-run psn_login.py to get a fresh token.", http_code);
		else
			set_err(err_buf, err_size, "PSN token refresh failed: %s", curl_easy_strerror(res));
		app_log("PSN: %s", err_buf ? err_buf : "token refresh failed");
		free(resp.data);
		return false;
	}

	json_tokener *tok = json_tokener_new();
	json_object *root = tok && resp.data ? json_tokener_parse_ex(tok, resp.data, (int)resp.size) : NULL;
	json_tokener_free(tok);
	free(resp.data);

	if(!root)
	{
		set_err(err_buf, err_size, "PSN token refresh returned invalid JSON.");
		return false;
	}

	json_object *at_obj = NULL, *rt_obj = NULL;
	if(!json_object_object_get_ex(root, "access_token", &at_obj) || !json_object_is_type(at_obj, json_type_string))
	{
		json_object_put(root);
		set_err(err_buf, err_size, "PSN token response did not contain access_token.");
		return false;
	}

	snprintf(cfg->psn_auth_token, sizeof(cfg->psn_auth_token), "%s", json_object_get_string(at_obj));
	if(json_object_object_get_ex(root, "refresh_token", &rt_obj) && json_object_is_type(rt_obj, json_type_string))
	{
		const char *new_rt = json_object_get_string(rt_obj);
		if(new_rt && new_rt[0])
			snprintf(cfg->psn_refresh_token, sizeof(cfg->psn_refresh_token), "%s", new_rt);
	}
	json_object_put(root);

	app_log("PSN: OAuth2 access token refreshed successfully");
	if(!config_save(cfg))
		app_log("PSN: warning - could not save refreshed token to %s", cfg->path);
	return true;
}

bool psn_ensure_token(Config *cfg, char *err_buf, size_t err_size)
{
	if(cfg->psn_refresh_token[0])
	{
		if(psn_refresh_token(cfg, err_buf, err_size))
			return true;
		if(cfg->psn_auth_token[0])
		{
			app_log("PSN: refresh failed, falling back to existing access token");
			return true;
		}
		return false;
	}
	if(cfg->psn_auth_token[0])
		return true;
	set_err(err_buf, err_size, "No PSN token in chiaki.cfg.\n\nRun psn_login.py or export_keys.py on your Mac/PC and import chiaki.cfg.");
	return false;
}

int psn_discover_consoles(Config *cfg, char *err_buf, size_t err_size)
{
	if(!psn_ensure_token(cfg, err_buf, err_size))
		return -1;

	ChiakiHolepunchDeviceInfo *devices = NULL;
	size_t count = 0;
	ChiakiErrorCode err = chiaki_holepunch_list_devices(
		cfg->psn_auth_token, CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5, &devices, &count, &app_log_ctx);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		set_err(err_buf, err_size, "Could not list PS5 consoles from PSN (%s).", chiaki_error_string(err));
		app_log("PSN: list_devices failed: %s", chiaki_error_string(err));
		return -1;
	}

	app_log("PSN: found %u PS5 console(s) on account", (unsigned)count);
	int updated = 0;
	for(size_t i = 0; i < count; i++)
	{
		const ChiakiHolepunchDeviceInfo *d = &devices[i];
		int idx = config_find_host_by_duid(cfg, d->device_uid);
		if(idx < 0)
		{
			// Try matching an existing PS5 without a DUID yet (by name or first PS5 without DUID)
			for(int j = 0; j < cfg->host_count; j++)
			{
				if(cfg->hosts[j].ps5 && !cfg->hosts[j].have_duid
					&& (d->device_name[0] && !strcasecmp(cfg->hosts[j].name, d->device_name)))
				{
					idx = j;
					break;
				}
			}
			if(idx < 0 && count == 1)
			{
				for(int j = 0; j < cfg->host_count; j++)
				{
					if(cfg->hosts[j].ps5 && !cfg->hosts[j].have_duid)
					{
						idx = j;
						break;
					}
				}
			}
		}

		ConfigHost *h = idx >= 0 ? &cfg->hosts[idx] : config_add_host(cfg);
		if(!h)
			continue;

		h->ps5 = true;
		memcpy(h->duid, d->device_uid, sizeof(h->duid));
		h->have_duid = true;
		if(!h->name[0] && d->device_name[0])
			snprintf(h->name, sizeof(h->name), "%.31s", d->device_name);
		if(!h->name[0])
			snprintf(h->name, sizeof(h->name), "PS5 (PSN)");
		if(!h->host[0])
			h->use_psn = true;
		updated++;
	}

	chiaki_holepunch_free_device_list(&devices);
	if(updated > 0)
		config_save(cfg);
	return (int)count;
}

// ---------------------------------------------------------------------------
// Async PSN holepunch connection (keeps PS3 display/XMB dialog alive)

typedef struct
{
	Config *cfg;
	ConfigHost *host;
	ChiakiHolepunchSession session;
	volatile const char *stage;
	volatile bool done;
	volatile bool ok;
	char err[256];
} PsnConnectCtx;

static void *psn_connect_thread(void *arg)
{
	PsnConnectCtx *ctx = arg;
	Config *cfg = ctx->cfg;
	ConfigHost *host = ctx->host;

	ctx->stage = "Refreshing PSN token...";
	if(!psn_ensure_token(cfg, ctx->err, sizeof(ctx->err)))
		goto fail;

	if(host->ps5 && !host->have_duid)
	{
		ctx->stage = "Finding your PS5 on PSN...";
		int found = psn_discover_consoles(cfg, ctx->err, sizeof(ctx->err));
		if(found < 0)
			goto fail;
		if(!host->have_duid)
		{
			set_err(ctx->err, sizeof(ctx->err),
				"No PS5 with Remote Play enabled was found on your PSN account.\n\n"
				"Make sure Remote Play is enabled on the PS5 and that you signed in with the same PSN account.");
			goto fail;
		}
	}

	ctx->stage = "Connecting to PSN signaling...";
	ChiakiHolepunchSession hp = chiaki_holepunch_session_init(cfg->psn_auth_token, &app_log_ctx);
	if(!hp)
	{
		set_err(ctx->err, sizeof(ctx->err), "Could not initialize PSN holepunch session.");
		goto fail;
	}
	ctx->session = hp;

	if(cfg->port_guessing)
		chiaki_holepunch_session_force_port_guessing(hp, true);

	chiaki_holepunch_upnp_discover(hp);

	ChiakiErrorCode err = chiaki_holepunch_session_create(hp);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		set_err(ctx->err, sizeof(ctx->err), "PSN session create failed (%s).", chiaki_error_string(err));
		goto fail_hp;
	}

	ctx->stage = "Querying STUN & creating offer...";
	err = holepunch_session_create_offer(hp);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		set_err(ctx->err, sizeof(ctx->err), "Could not create STUN/NAT offer (%s).", chiaki_error_string(err));
		goto fail_hp;
	}

	ctx->stage = host->ps5 ? "Waking up your PS5 via PSN..." : "Waking up your PS4 via PSN...";
	ChiakiHolepunchConsoleType ctype = host->ps5 ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4;
	err = chiaki_holepunch_session_start(hp, host->duid, ctype);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(err == CHIAKI_ERR_HOST_DOWN)
			set_err(ctx->err, sizeof(ctx->err),
				"Your %s did not respond via PSN.\n\nMake sure it is turned on or in Rest Mode with Remote Play enabled.",
				host->ps5 ? "PS5" : "PS4");
		else
			set_err(ctx->err, sizeof(ctx->err), "PSN session start failed (%s).", chiaki_error_string(err));
		goto fail_hp;
	}

	ctx->stage = "Punching NAT hole to console...";
	err = chiaki_holepunch_session_punch_hole(hp, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		set_err(ctx->err, sizeof(ctx->err),
			"NAT hole punching failed (%s).\n\nIf you are behind a strict/symmetric NAT, try enabling \"NAT port guessing\" in Settings.",
			chiaki_error_string(err));
		goto fail_hp;
	}

	ctx->ok = true;
	ctx->done = true;
	return NULL;

fail_hp:
	chiaki_holepunch_session_fini(hp);
	ctx->session = NULL;
fail:
	ctx->ok = false;
	ctx->done = true;
	return NULL;
}

ChiakiHolepunchSession psn_connect_console(Config *cfg, ConfigHost *host, char *err_buf, size_t err_size)
{
	if(!cfg->have_account_id)
	{
		set_err(err_buf, err_size, "PSN account ID is not set. Import chiaki.cfg or enter it in Settings.");
		return NULL;
	}

	PsnConnectCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = cfg;
	ctx.host = host;
	ctx.stage = "Connecting to PSN...";

	ChiakiThread thread;
	if(chiaki_thread_create(&thread, psn_connect_thread, &ctx) != CHIAKI_ERR_SUCCESS)
	{
		set_err(err_buf, err_size, "Could not start PSN connection thread.");
		return NULL;
	}

	const char *shown_stage = NULL;
	bool canceled = false;
	while(!ctx.done)
	{
		const char *cur = (const char *)ctx.stage;
		if(cur != shown_stage)
		{
			shown_stage = cur;
			dialog_status(cur);
		}
		if(!keep_display_alive() && !canceled)
		{
			canceled = true;
			if(ctx.session)
				chiaki_holepunch_main_thread_cancel(ctx.session, true);
		}
	}
	chiaki_thread_join(&thread, NULL);

	if(!ctx.ok)
	{
		if(err_buf && err_size)
			snprintf(err_buf, err_size, "%s", ctx.err[0] ? ctx.err : "PSN connection failed.");
		return NULL;
	}
	return ctx.session;
}
