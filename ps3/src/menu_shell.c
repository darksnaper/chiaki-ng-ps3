// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PS3 side of the start menu: reads the pad, keeps a live list of PS4s that
// answer discovery, draws the menu into an image shown by video.c, and carries
// out the menu's actions (registration, saving, quitting).

#include "menu_shell.h"

#include "app.h"
#include "discover.h"
#include "input.h"
#include "menu.h"
#include "psn.h"
#include "video.h"

#include <chiaki/discovery.h>
#include <chiaki/regist.h>
#include <chiaki/thread.h>
#include <chiaki/time.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>
#include <net/netctl.h>
#include <sysutil/sysutil.h>

#define SEARCH_INTERVAL_MS 2000
#define HOST_TIMEOUT_MS    6000

static Menu menu;
static bool menu_initialized;
static uint32_t pixels[UI_W * UI_H];

// ---------------------------------------------------------------------------
// live discovery

typedef struct
{
	DiscoveredHost host;
	uint64_t seen_ms;
} SeenHost;

static ChiakiMutex seen_mutex;
static bool seen_mutex_ok;
static SeenHost seen[MENU_HOSTS_MAX];
static int seen_count;

static ChiakiDiscovery discovery;
static ChiakiDiscoveryThread discovery_thread;
static bool discovery_running;
static uint64_t last_search_ms;
static char bcast[32];
static bool have_bcast;

static void copy_str(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void discovery_cb(ChiakiDiscoveryHost *dh, void *user)
{
	(void)user;
	if(!dh->host_addr)
		return;
	struct in_addr ip;
	if(!inet_aton(dh->host_addr, &ip) || ip.s_addr == 0)
		return;
	chiaki_mutex_lock(&seen_mutex);
	int idx = -1;
	for(int i = 0; i < seen_count; i++)
	{
		bool same_id = dh->host_id && seen[i].host.id[0] && !strcasecmp(seen[i].host.id, dh->host_id);
		bool same_addr = !strcmp(seen[i].host.addr, dh->host_addr);
		if(same_id || same_addr)
		{
			idx = i;
			break;
		}
	}
	if(idx < 0 && seen_count < MENU_HOSTS_MAX)
		idx = seen_count++;
	if(idx >= 0)
	{
		SeenHost *s = &seen[idx];
		s->host.ps5 = chiaki_discovery_host_is_ps5(dh);
		copy_str(s->host.name, sizeof(s->host.name), dh->host_name ? dh->host_name : (s->host.ps5 ? "PS5" : "PS4"));
		copy_str(s->host.addr, sizeof(s->host.addr), dh->host_addr);
		copy_str(s->host.id, sizeof(s->host.id), dh->host_id);
		s->host.standby = dh->state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY;
		s->seen_ms = chiaki_time_now_monotonic_ms();
	}
	chiaki_mutex_unlock(&seen_mutex);
}

static void discovery_start(void)
{
	if(!seen_mutex_ok)
	{
		chiaki_mutex_init(&seen_mutex, false);
		seen_mutex_ok = true;
	}
	if(chiaki_discovery_init(&discovery, &app_log_ctx, AF_INET) != CHIAKI_ERR_SUCCESS)
	{
		app_log("Menu: discovery init failed");
		return;
	}
	if(chiaki_discovery_thread_start(&discovery_thread, &discovery, discovery_cb, NULL) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_discovery_fini(&discovery);
		app_log("Menu: discovery thread failed");
		return;
	}
	have_bcast = discover_subnet_broadcast(bcast, sizeof(bcast));
	last_search_ms = 0;
	discovery_running = true;
}

static void discovery_stop(void)
{
	if(!discovery_running)
		return;
	chiaki_discovery_thread_stop(&discovery_thread);
	chiaki_discovery_fini(&discovery);
	discovery_running = false;
}

static void discovery_tick(Config *cfg)
{
	if(!discovery_running)
		return;
	uint64_t now = chiaki_time_now_monotonic_ms();
	if(!last_search_ms || now - last_search_ms >= SEARCH_INTERVAL_MS)
	{
		last_search_ms = now;
		for(int ps5 = 0; ps5 <= 1; ps5++)
		{
			discover_send_search(&discovery, "255.255.255.255", ps5);
			if(have_bcast)
				discover_send_search(&discovery, bcast, ps5);
			// Also ask saved consoles directly (on both ports, so a console added by IP
			// is recognised as PS4 or PS5), in case broadcasts are filtered.
			for(int i = 0; i < cfg->host_count; i++)
			{
				struct in_addr ip;
				if(cfg->hosts[i].host[0] && inet_aton(cfg->hosts[i].host, &ip))
					discover_send_search(&discovery, cfg->hosts[i].host, ps5);
			}
		}
	}

	DiscoveredHost list[MENU_HOSTS_MAX];
	int n = 0;
	chiaki_mutex_lock(&seen_mutex);
	for(int i = 0; i < seen_count; i++)
		if(now - seen[i].seen_ms <= HOST_TIMEOUT_MS)
			list[n++] = seen[i].host;
	chiaki_mutex_unlock(&seen_mutex);

	// Saved consoles without a name (e.g. from an older config) take the name they report.
	bool named = false;
	for(int i = 0; i < cfg->host_count; i++)
	{
		ConfigHost *h = &cfg->hosts[i];
		if(h->name[0])
			continue;
		for(int j = 0; j < n; j++)
		{
			bool same = (h->host_id[0] && strcasecmp(h->host_id, list[j].id) == 0)
				|| (!h->host_id[0] && h->host[0] && strcmp(h->host, list[j].addr) == 0);
			if(same && list[j].name[0])
			{
				copy_str(h->name, sizeof(h->name), list[j].name);
				named = true;
				break;
			}
		}
	}
	if(named)
	{
		if(!config_save(cfg))
			app_log("Menu: could not save console names");
	}
	menu_set_discovered(&menu, list, n);
}

// ---------------------------------------------------------------------------
// registration (runs asynchronously while the display keeps going)

typedef struct
{
	volatile bool done;
	volatile bool success;
	ChiakiRegisteredHost host;
} RegistResult;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	RegistResult *res = user;
	if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		res->host = *event->registered_host;
		res->success = true;
	}
	res->done = true;
}

static void do_register(Config *cfg, int menu_host, uint32_t pin)
{
	if(menu_host < 0 || menu_host >= menu.host_count)
		return;
	MenuHost mh = menu.hosts[menu_host]; // copy: the list may be rebuilt meanwhile
	app_log("Registering with %s \"%s\" at %s", mh.ps5 ? "PS5" : "PS4", mh.name, mh.addr);
	dialog_status(mh.ps5 ? "Registering with the PS5..." : "Registering with the PS4...");

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	info.target = mh.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.host = mh.addr;
	info.broadcast = false;
	memcpy(info.psn_account_id, cfg->psn_account_id, sizeof(info.psn_account_id));
	info.pin = pin;

	RegistResult res;
	memset(&res, 0, sizeof(res));
	ChiakiRegist regist;
	if(chiaki_regist_start(&regist, &app_log_ctx, &info, regist_cb, &res) != CHIAKI_ERR_SUCCESS)
	{
		dialog_wait_ok(true, "Could not start the registration.");
		return;
	}
	uint64_t start = chiaki_time_now_monotonic_ms();
	while(!res.done && !app_exit && chiaki_time_now_monotonic_ms() - start < 60 * 1000)
		keep_display_alive();
	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	dialog_close();

	if(!res.success)
	{
		if(!app_exit)
			dialog_wait_ok(true, "Registration failed.\n\nCheck the PIN (it is only valid for a few minutes) and that the console still shows the PIN screen.");
		return;
	}

	ConfigHost *ch = mh.cfg_index >= 0 && mh.cfg_index < cfg->host_count ? &cfg->hosts[mh.cfg_index] : NULL;
	if(!ch)
	{
		int existing = config_find_host(cfg, mh.id, mh.addr);
		ch = existing >= 0 ? &cfg->hosts[existing] : config_add_host(cfg);
	}
	if(!ch)
	{
		dialog_wait_ok(true, "Registered, but the console list is full: remove a console first.");
		return;
	}
	copy_str(ch->host, sizeof(ch->host), mh.addr);
	ch->ps5 = mh.ps5;
	if(mh.id[0])
		copy_str(ch->host_id, sizeof(ch->host_id), mh.id);
	config_host_set_registration(ch, &res.host);
	app_log("Registered with \"%s\"", ch->name);
	menu_rebuild(&menu);
	char note[128];
	snprintf(note, sizeof(note), "Registered with %s. Press X to connect.", ch->name);
	menu_set_note(&menu, note);
	if(!config_save(cfg))
		dialog_wait_ok(true, "Registered, but the settings could not be saved to\n%s", cfg->path);
}

// ---------------------------------------------------------------------------
// PSN account id / USB import

#define ACCOUNT_HELP \
	"Your PSN account ID is a short code like \"AbCdEfGhIjk=\" (or a long number). " \
	"chiaki-ng on a PC or Mac shows it after you log in to PSN in its settings."

static void enter_account_id(Config *cfg, int register_host)
{
	if(register_host >= 0 && !dialog_yes_no(
				"Pairing a console needs your PSN account ID once.\n\n" ACCOUNT_HELP "\n\nEnter it now?"))
		return;
	char current[32], text[80];
	config_format_account_id(cfg, current, sizeof(current));
	for(;;)
	{
		int r = osk_input("PSN account ID", current, text, sizeof(text));
		if(r < 0)
		{
			dialog_wait_ok(true, "The on-screen keyboard could not be opened.\n\nYou can also add the line\n"
					"psn_account_id=...\nto %s", cfg->path);
			return;
		}
		if(r == 0)
			return;
		uint8_t id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
		if(config_parse_account_id(text, id))
		{
			memcpy(cfg->psn_account_id, id, sizeof(cfg->psn_account_id));
			cfg->have_account_id = true;
			break;
		}
		app_log("Menu: account id entry not valid (%u characters)", (unsigned)strlen(text));
		if(!dialog_yes_no("That does not look like a PSN account ID.\n\n" ACCOUNT_HELP
					"\n\nCapital and small letters matter. Try again?"))
			return;
		copy_str(current, sizeof(current), text);
	}
	if(!config_save(cfg))
		menu_set_note(&menu, "Could not save the settings.");
	else
		menu_set_note(&menu, "PSN account ID saved.");
	app_log("Menu: PSN account id set from the on-screen keyboard");
	if(register_host >= 0)
		menu_begin_register(&menu, register_host);
}

static void import_from_usb(Config *cfg)
{
	char path[64];
	if(!config_find_usb(path, sizeof(path)))
	{
		dialog_wait_ok(false, "No chiaki.cfg found on a USB stick.\n\nPut the file in the top folder of the stick "
				"(not inside another folder) and try again.");
		return;
	}
	char question[256];
	snprintf(question, sizeof(question), "Found %s.\n\nReplace all settings and consoles on this PS3 with the ones "
			"from this file?", path);
	if(!dialog_yes_no(question))
		return;
	if(!config_import(cfg, path))
	{
		dialog_wait_ok(true, "Could not read %s", path);
		return;
	}
	bool saved = config_save(cfg);
	app_log("Menu: imported %s, %d console(s), account id %s", path, cfg->host_count, cfg->have_account_id ? "set" : "not set");
	menu.cfg = cfg;
	menu.screen = MENU_SCREEN_HOSTS;
	menu_rebuild(&menu);
	char note[160];
	snprintf(note, sizeof(note), saved ? "Imported %d console(s) from the USB stick." : "Imported %d console(s), but could not save them.",
			cfg->host_count);
	menu_set_note(&menu, note);
}

static void discover_psn_consoles(Config *cfg)
{
	if(!config_has_psn(cfg))
	{
		dialog_wait_ok(false, "No PSN token found in chiaki.cfg.\n\n"
			"Run python3 ps3/tools/psn_login.py on your Mac or PC to sign in to PSN and create chiaki.cfg, "
			"then import it from a USB stick.");
		return;
	}
	dialog_status("Searching your PSN account for PS5 consoles...");
	char err[256] = {0};
	int found = psn_discover_consoles(cfg, err, sizeof(err));
	dialog_close();
	if(found < 0)
	{
		dialog_wait_ok(true, "%s", err[0] ? err : "Could not query PSN for consoles.");
		return;
	}
	menu.cfg = cfg;
	menu.screen = MENU_SCREEN_HOSTS;
	menu_rebuild(&menu);
	char note[160];
	if(found == 0)
		snprintf(note, sizeof(note), "No PS5 consoles found on this PSN account.");
	else
		snprintf(note, sizeof(note), "Found %d PS5 console(s) via PSN. Press X to connect.", found);
	menu_set_note(&menu, note);
}

// ---------------------------------------------------------------------------

int menu_shell_run(Config *cfg)
{
	if(!menu_initialized)
	{
		menu_init(&menu, cfg);
		menu_initialized = true;
	}
	else
	{
		menu.cfg = cfg;
		menu.screen = MENU_SCREEN_HOSTS;
		menu_rebuild(&menu);
	}
	union net_ctl_info ipinfo;
	if(netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &ipinfo) == 0)
		copy_str(menu.ps3_ip, sizeof(menu.ps3_ip), ipinfo.ip_address);
	menu.dirty = true;

	discovery_start();
	Canvas canvas = { pixels, UI_W, UI_H };
	int result = -1;
	input_menu_reset(); // buttons still held from the stream are not new presses

	while(!app_exit)
	{
		sysUtilCheckCallback();
		MenuAction act = menu_handle(&menu, input_menu_events());
		switch(act.type)
		{
			case MENU_ACT_CONNECT:
				if(act.host >= 0 && act.host < cfg->host_count)
				{
					result = act.host;
					goto done;
				}
				break;
			case MENU_ACT_REGISTER:
				do_register(cfg, act.host, act.pin);
				break;
			case MENU_ACT_SAVE:
				if(!config_save(cfg))
					menu_set_note(&menu, "Could not save the settings.");
				break;
			case MENU_ACT_QUIT:
				if(dialog_yes_no("Quit Chiaki?"))
					goto done;
				menu.dirty = true;
				break;
			case MENU_ACT_MESSAGE:
				dialog_wait_ok(false, "%s", act.text);
				menu.dirty = true;
				break;
			case MENU_ACT_ACCOUNT:
				enter_account_id(cfg, act.host);
				input_menu_reset();
				menu.dirty = true;
				break;
			case MENU_ACT_IMPORT:
				import_from_usb(cfg);
				input_menu_reset();
				menu.dirty = true;
				break;
			case MENU_ACT_PSN_DISCOVER:
				discover_psn_consoles(cfg);
				input_menu_reset();
				menu.dirty = true;
				break;
			case MENU_ACT_NONE:
				break;
		}

		discovery_tick(cfg);
		if(menu.cfg_changed)
		{
			menu.cfg_changed = false;
			if(!config_save(cfg))
				app_log("Menu: could not save the console type change");
		}
		if(menu.dirty)
		{
			menu_draw(&menu, &canvas);
			video_display_show_ui(pixels);
		}
		video_display_present();
	}

done:
	discovery_stop();
	return result;
}
