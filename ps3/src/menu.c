// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "menu.h"
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// colours / layout

#define COL_TEXT      0xf2f4f8
#define COL_DIM       0x9aa6b8
#define COL_ACCENT    0x6fb6ff
#define COL_READY     0x5fd38a
#define COL_STANDBY   0xf0b44c
#define COL_MISSING   0x6c7480
#define COL_WARN      0xffc46b

#define MARGIN_X      96
#define LIST_Y        156
#define ROW_H         84
#define LIST_ROWS     5
#define HINT_Y        646

enum { OPT_CONNECT, OPT_REGISTER, OPT_SET_IP, OPT_TYPE, OPT_PSN_MODE, OPT_REMOVE };

static const int res_values[] = { 360, 540, 720 };
static const int fps_values[] = { 30, 60 };
static const int bitrate_values[] = { 0, 3000, 5000, 7500, 10000, 15000 };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { SET_RESOLUTION, SET_FPS, SET_BITRATE, SET_PACING, SET_LOW_LATENCY, SET_VIBRATION, SET_PORT_GUESSING, SET_DUMP, SET_VERBOSE, SET_ACCOUNT, SET_PSN_DISCOVER, SET_IMPORT, SET_COUNT };

// ---------------------------------------------------------------------------
// model

static void copy_str(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);
	if(n >= size)
		n = size - 1;
	memmove(dst, src, n);
	dst[n] = '\0';
}

void menu_set_note(Menu *m, const char *note)
{
	copy_str(m->note, sizeof(m->note), note ? note : "");
	m->dirty = true;
}

void menu_rebuild(Menu *m)
{
	char prev_id[32] = "", prev_addr[64] = "";
	if(m->sel >= 0 && m->sel < m->host_count)
	{
		copy_str(prev_id, sizeof(prev_id), m->hosts[m->sel].id);
		copy_str(prev_addr, sizeof(prev_addr), m->hosts[m->sel].addr);
	}

	m->host_count = 0;
	bool used[MENU_HOSTS_MAX] = { false };
	for(int i = 0; i < m->cfg->host_count && m->host_count < MENU_HOSTS_MAX; i++)
	{
		const ConfigHost *ch = &m->cfg->hosts[i];
		MenuHost *h = &m->hosts[m->host_count++];
		memset(h, 0, sizeof(*h));
		h->cfg_index = i;
		h->ps5 = ch->ps5;
		copy_str(h->name, sizeof(h->name), ch->name[0] ? ch->name : (ch->ps5 ? "PS5" : "PS4"));
		copy_str(h->addr, sizeof(h->addr), ch->host);
		copy_str(h->id, sizeof(h->id), ch->host_id);
		for(int d = 0; d < m->discovered_count; d++)
		{
			const DiscoveredHost *dh = &m->discovered[d];
			bool same = (ch->host_id[0] && dh->id[0] && !strcasecmp(ch->host_id, dh->id))
				|| (!ch->host_id[0] && ch->host[0] && !strcmp(ch->host, dh->addr));
			if(!same || used[d])
				continue;
			used[d] = true;
			h->discovered = true;
			h->standby = dh->standby;
			if(dh->ps5 != ch->ps5)
			{
				// discovery knows the console type better (e.g. added by IP as a PS4)
				m->cfg->hosts[i].ps5 = dh->ps5;
				h->ps5 = dh->ps5;
				m->cfg_changed = true;
			}
			copy_str(h->addr, sizeof(h->addr), dh->addr);
			if(!ch->name[0] && dh->name[0])
				copy_str(h->name, sizeof(h->name), dh->name);
			if(!h->id[0])
				copy_str(h->id, sizeof(h->id), dh->id);
			break;
		}
	}
	for(int d = 0; d < m->discovered_count && m->host_count < MENU_HOSTS_MAX; d++)
	{
		if(used[d])
			continue;
		const DiscoveredHost *dh = &m->discovered[d];
		MenuHost *h = &m->hosts[m->host_count++];
		memset(h, 0, sizeof(*h));
		h->cfg_index = -1;
		h->discovered = true;
		h->standby = dh->standby;
		h->ps5 = dh->ps5;
		copy_str(h->name, sizeof(h->name), dh->name[0] ? dh->name : (dh->ps5 ? "PS5" : "PS4"));
		copy_str(h->addr, sizeof(h->addr), dh->addr);
		copy_str(h->id, sizeof(h->id), dh->id);
	}

	// keep the same console selected
	for(int i = 0; i < m->host_count; i++)
	{
		if((prev_id[0] && !strcasecmp(prev_id, m->hosts[i].id)) || (!prev_id[0] && prev_addr[0] && !strcmp(prev_addr, m->hosts[i].addr)))
		{
			m->sel = i;
			break;
		}
	}
	if(m->sel >= m->host_count)
		m->sel = m->host_count - 1;
	if(m->sel < 0)
		m->sel = 0;
	m->dirty = true;
}

void menu_init(Menu *m, Config *cfg)
{
	memset(m, 0, sizeof(*m));
	m->cfg = cfg;
	m->screen = MENU_SCREEN_HOSTS;
	menu_rebuild(m);
}

void menu_set_discovered(Menu *m, const DiscoveredHost *list, int count)
{
	if(count > MENU_HOSTS_MAX)
		count = MENU_HOSTS_MAX;
	bool changed = count != m->discovered_count || memcmp(list, m->discovered, (size_t)count * sizeof(*list)) != 0;
	if(!changed)
		return;
	memcpy(m->discovered, list, (size_t)count * sizeof(*list));
	m->discovered_count = count;
	menu_rebuild(m);
}

static bool host_registered(const Menu *m, const MenuHost *h)
{
	return h->cfg_index >= 0 && config_host_registered(&m->cfg->hosts[h->cfg_index]);
}

static bool host_connectable(const Menu *m, const MenuHost *h)
{
	return h->cfg_index >= 0 && config_host_connectable(m->cfg, &m->cfg->hosts[h->cfg_index]);
}

// ---------------------------------------------------------------------------
// number entry

static void num_start(Menu *m, MenuNumberPurpose purpose, int host, const char *initial_ip)
{
	m->num.purpose = purpose;
	m->num.host = host;
	m->num.cursor = 0;
	if(purpose == MENU_NUM_PIN)
	{
		m->num.len = 8;
		memset(m->num.digits, '0', 8);
	}
	else
	{
		m->num.len = 12;
		unsigned a = 192, b = 168, c = 1, d = 0;
		if(initial_ip && *initial_ip)
			sscanf(initial_ip, "%u.%u.%u.%u", &a, &b, &c, &d);
		char tmp[16];
		snprintf(tmp, sizeof(tmp), "%03u%03u%03u%03u", a % 1000, b % 1000, c % 1000, d % 1000);
		memcpy(m->num.digits, tmp, 12);
		m->num.cursor = 9; // usually only the last part differs
	}
	m->num.digits[m->num.len] = '\0';
	m->note[0] = '\0';
	m->screen = MENU_SCREEN_NUMBER;
	m->dirty = true;
}

static bool num_ip(const Menu *m, char *out, size_t out_size)
{
	unsigned o[4];
	for(int k = 0; k < 4; k++)
	{
		o[k] = (unsigned)((m->num.digits[k * 3] - '0') * 100 + (m->num.digits[k * 3 + 1] - '0') * 10 + (m->num.digits[k * 3 + 2] - '0'));
		if(o[k] > 255)
			return false;
	}
	if(o[0] == 0)
		return false;
	snprintf(out, out_size, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
	return true;
}

static MenuAction num_handle(Menu *m, uint32_t ev)
{
	MenuAction act = { MENU_ACT_NONE, -1, 0, "" };
	if(ev & MENU_EV_LEFT)
		m->num.cursor = (m->num.cursor + m->num.len - 1) % m->num.len;
	if(ev & MENU_EV_RIGHT)
		m->num.cursor = (m->num.cursor + 1) % m->num.len;
	if(ev & (MENU_EV_UP | MENU_EV_DOWN))
	{
		char *d = &m->num.digits[m->num.cursor];
		int v = *d - '0';
		v = (ev & MENU_EV_UP) ? (v + 1) % 10 : (v + 9) % 10;
		*d = (char)('0' + v);
	}
	if(ev & MENU_EV_CIRCLE)
	{
		m->screen = MENU_SCREEN_HOSTS;
		menu_set_note(m, "");
	}
	else if(ev & MENU_EV_CROSS)
	{
		if(m->num.purpose == MENU_NUM_PIN)
		{
			act.type = MENU_ACT_REGISTER;
			act.host = m->num.host;
			act.pin = (uint32_t)strtoul(m->num.digits, NULL, 10);
			m->screen = MENU_SCREEN_HOSTS;
		}
		else
		{
			char ip[32];
			if(!num_ip(m, ip, sizeof(ip)))
			{
				menu_set_note(m, "That is not a valid IP address (each part must be 0-255).");
				return act;
			}
			if(m->num.purpose == MENU_NUM_IP_ADD)
			{
				ConfigHost *h = config_add_host(m->cfg);
				if(!h)
				{
					menu_set_note(m, "The console list is full.");
					m->screen = MENU_SCREEN_HOSTS;
					return act;
				}
				copy_str(h->host, sizeof(h->host), ip);
				snprintf(h->name, sizeof(h->name), "PS4 (%.15s)", ip);
				menu_rebuild(m);
				m->sel = m->host_count - 1;
				for(int i = 0; i < m->host_count; i++)
					if(m->hosts[i].cfg_index == m->cfg->host_count - 1)
						m->sel = i;
				menu_set_note(m, "Console added (as PS4; a PS5 is recognised automatically when it answers). Press X to register.");
			}
			else if(m->num.host >= 0 && m->num.host < m->host_count && m->hosts[m->num.host].cfg_index >= 0)
			{
				ConfigHost *h = &m->cfg->hosts[m->hosts[m->num.host].cfg_index];
				copy_str(h->host, sizeof(h->host), ip);
				menu_rebuild(m);
				menu_set_note(m, "IP address saved.");
			}
			m->screen = MENU_SCREEN_HOSTS;
			act.type = MENU_ACT_SAVE;
		}
	}
	m->dirty = true;
	return act;
}

// ---------------------------------------------------------------------------
// screens: input

static void open_options(Menu *m)
{
	const MenuHost *h = &m->hosts[m->sel];
	m->opt_count = 0;
	if(host_connectable(m, h))
		m->opt_items[m->opt_count++] = OPT_CONNECT;
	m->opt_items[m->opt_count++] = OPT_REGISTER;
	if(h->cfg_index >= 0)
	{
		m->opt_items[m->opt_count++] = OPT_SET_IP;
		m->opt_items[m->opt_count++] = OPT_TYPE;
		m->opt_items[m->opt_count++] = OPT_PSN_MODE;
		m->opt_items[m->opt_count++] = OPT_REMOVE;
	}
	m->opt_sel = 0;
	m->note[0] = '\0';
	m->screen = MENU_SCREEN_OPTIONS;
	m->dirty = true;
}

static MenuAction activate_host(Menu *m)
{
	MenuAction act = { MENU_ACT_NONE, -1, 0, "" };
	const MenuHost *h = &m->hosts[m->sel];
	if(host_connectable(m, h))
	{
		act.type = MENU_ACT_CONNECT;
		act.host = h->cfg_index;
		return act;
	}
	if(!m->cfg->have_account_id)
	{
		act.type = MENU_ACT_ACCOUNT;
		act.host = m->sel;
		return act;
	}
	if(!h->addr[0])
	{
		menu_set_note(m, "This console has no IP address yet: use Options > Set IP address.");
		return act;
	}
	num_start(m, MENU_NUM_PIN, m->sel, NULL);
	return act;
}

void menu_begin_register(Menu *m, int host)
{
	if(host < 0 || host >= m->host_count || !m->cfg->have_account_id)
		return;
	m->sel = host;
	if(!m->hosts[host].addr[0])
	{
		m->screen = MENU_SCREEN_HOSTS;
		menu_set_note(m, "This console has no IP address yet: use Options > Set IP address.");
		return;
	}
	num_start(m, MENU_NUM_PIN, host, NULL);
}

static MenuAction hosts_handle(Menu *m, uint32_t ev)
{
	MenuAction act = { MENU_ACT_NONE, -1, 0, "" };
	if(ev & (MENU_EV_UP | MENU_EV_DOWN))
		m->note[0] = '\0';
	if(m->host_count > 0)
	{
		if(ev & MENU_EV_UP)
			m->sel = (m->sel + m->host_count - 1) % m->host_count;
		if(ev & MENU_EV_DOWN)
			m->sel = (m->sel + 1) % m->host_count;
		if(ev & MENU_EV_CROSS)
			act = activate_host(m);
		else if(ev & MENU_EV_TRIANGLE)
			open_options(m);
	}
	if(ev & MENU_EV_SQUARE)
	{
		char base[32] = "";
		if(m->ps3_ip[0])
			copy_str(base, sizeof(base), m->ps3_ip);
		num_start(m, MENU_NUM_IP_ADD, -1, base);
	}
	else if(ev & MENU_EV_START)
	{
		m->screen = MENU_SCREEN_SETTINGS;
		m->note[0] = '\0';
		m->set_sel = 0;
		m->settings_dirty = false;
	}
	else if(ev & MENU_EV_CIRCLE)
		act.type = MENU_ACT_QUIT;
	if(ev)
		m->dirty = true;
	return act;
}

static MenuAction options_handle(Menu *m, uint32_t ev)
{
	MenuAction act = { MENU_ACT_NONE, -1, 0, "" };
	if(ev & MENU_EV_UP)
		m->opt_sel = (m->opt_sel + m->opt_count - 1) % m->opt_count;
	if(ev & MENU_EV_DOWN)
		m->opt_sel = (m->opt_sel + 1) % m->opt_count;
	if(ev & MENU_EV_CIRCLE)
		m->screen = MENU_SCREEN_HOSTS;
	else if(ev & MENU_EV_CROSS)
	{
		MenuHost *h = &m->hosts[m->sel];
		m->screen = MENU_SCREEN_HOSTS;
		switch(m->opt_items[m->opt_sel])
		{
			case OPT_CONNECT:
				act.type = MENU_ACT_CONNECT;
				act.host = h->cfg_index;
				break;
			case OPT_REGISTER:
				if(!m->cfg->have_account_id || !h->addr[0])
					return activate_host(m); // shows the reason
				num_start(m, MENU_NUM_PIN, m->sel, NULL);
				break;
			case OPT_SET_IP:
				num_start(m, MENU_NUM_IP_EDIT, m->sel, h->addr[0] ? h->addr : m->ps3_ip);
				break;
			case OPT_TYPE:
			{
				ConfigHost *ch = &m->cfg->hosts[h->cfg_index];
				ch->ps5 = !ch->ps5;
				menu_rebuild(m);
				menu_set_note(m, ch->ps5 ? "Console type set to PS5." : "Console type set to PS4.");
				act.type = MENU_ACT_SAVE;
				break;
			}
			case OPT_PSN_MODE:
			{
				ConfigHost *ch = &m->cfg->hosts[h->cfg_index];
				ch->use_psn = !ch->use_psn;
				menu_rebuild(m);
				menu_set_note(m, ch->use_psn ? "Connection mode: PSN (Internet)." : "Connection mode: Auto (LAN first, then PSN).");
				act.type = MENU_ACT_SAVE;
				break;
			}
			case OPT_REMOVE:
				config_remove_host(m->cfg, h->cfg_index);
				menu_rebuild(m);
				menu_set_note(m, "Console removed.");
				act.type = MENU_ACT_SAVE;
				break;
		}
	}
	if(ev)
		m->dirty = true;
	return act;
}

static int index_of(const int *values, int count, int v)
{
	for(int i = 0; i < count; i++)
		if(values[i] == v)
			return i;
	return count - 1;
}

static MenuAction settings_handle(Menu *m, uint32_t ev)
{
	MenuAction act = { MENU_ACT_NONE, -1, 0, "" };
	Config *cfg = m->cfg;
	if(ev & MENU_EV_UP)
		m->set_sel = (m->set_sel + SET_COUNT - 1) % SET_COUNT;
	if(ev & MENU_EV_DOWN)
		m->set_sel = (m->set_sel + 1) % SET_COUNT;
	int step = (ev & MENU_EV_RIGHT) ? 1 : (ev & MENU_EV_LEFT) ? -1 : 0;
	if((ev & MENU_EV_CROSS) && m->set_sel != SET_ACCOUNT && m->set_sel != SET_PSN_DISCOVER && m->set_sel != SET_IMPORT)
		step = 1;
	if(step)
	{
		int i;
		switch(m->set_sel)
		{
			case SET_RESOLUTION:
				i = index_of(res_values, COUNT(res_values), cfg->resolution);
				cfg->resolution = res_values[(i + step + COUNT(res_values)) % COUNT(res_values)];
				break;
			case SET_FPS:
				i = index_of(fps_values, COUNT(fps_values), cfg->fps);
				cfg->fps = fps_values[(i + step + COUNT(fps_values)) % COUNT(fps_values)];
				break;
			case SET_BITRATE:
				i = index_of(bitrate_values, COUNT(bitrate_values), cfg->bitrate);
				cfg->bitrate = bitrate_values[(i + step + COUNT(bitrate_values)) % COUNT(bitrate_values)];
				break;
			case SET_PACING:
				cfg->pacing = (cfg->pacing + step + 3) % 3;
				break;
			case SET_LOW_LATENCY:
				cfg->low_latency = !cfg->low_latency;
				break;
			case SET_VIBRATION:
				cfg->vibration = !cfg->vibration;
				break;
			case SET_PORT_GUESSING:
				cfg->port_guessing = !cfg->port_guessing;
				break;
			case SET_DUMP:
				cfg->dump_stream = !cfg->dump_stream;
				break;
			case SET_VERBOSE:
				cfg->verbose_log = !cfg->verbose_log;
				break;
			default:
				step = 0;
				break;
		}
		if(step)
			m->settings_dirty = true;
	}
	if((ev & MENU_EV_CROSS) && m->set_sel == SET_ACCOUNT)
		act.type = MENU_ACT_ACCOUNT;
	if((ev & MENU_EV_CROSS) && m->set_sel == SET_PSN_DISCOVER)
		act.type = MENU_ACT_PSN_DISCOVER;
	if((ev & MENU_EV_CROSS) && m->set_sel == SET_IMPORT)
		act.type = MENU_ACT_IMPORT;
	if(act.type != MENU_ACT_NONE)
	{
		m->dirty = true;
		return act; // stay in the settings
	}
	if(ev & (MENU_EV_CIRCLE | MENU_EV_START))
	{
		m->screen = MENU_SCREEN_HOSTS;
		if(m->settings_dirty)
		{
			act.type = MENU_ACT_SAVE;
			menu_set_note(m, "Settings saved.");
		}
	}
	if(ev)
		m->dirty = true;
	return act;
}

MenuAction menu_handle(Menu *m, uint32_t ev)
{
	MenuAction none = { MENU_ACT_NONE, -1, 0, "" };
	if(!ev)
		return none;
	switch(m->screen)
	{
		case MENU_SCREEN_HOSTS: return hosts_handle(m, ev);
		case MENU_SCREEN_OPTIONS: return options_handle(m, ev);
		case MENU_SCREEN_SETTINGS: return settings_handle(m, ev);
		case MENU_SCREEN_NUMBER: return num_handle(m, ev);
	}
	return none;
}

// ---------------------------------------------------------------------------
// drawing

static void draw_background(Canvas *c)
{
	ui_vgradient(c, 0, 0, UI_W, UI_H, 0x16305e, 0x050a18);
	// soft light ribbons, a nod to the XMB wave
	ui_wave(c, 470, 38, 0.0042f, 0.6f, 70, 0x8fb8ff, 26);
	ui_wave(c, 500, 52, 0.0031f, 2.1f, 46, 0x6f9cf0, 20);
	ui_wave(c, 455, 24, 0.0057f, 4.0f, 30, 0xb8d4ff, 16);
}

static void draw_header(Menu *m, Canvas *c, const char *title)
{
	ui_text(c, &font_title, MARGIN_X, 54, title, COL_TEXT);
	if(m->ps3_ip[0])
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "PS3  %s", m->ps3_ip);
		int w = ui_text_width(&font_small, buf);
		ui_text(c, &font_small, UI_W - MARGIN_X - w, 70, buf, COL_DIM);
	}
	ui_blend(c, MARGIN_X, 118, UI_W - 2 * MARGIN_X, 2, 0xffffff, 60);
}

static void draw_hints(Canvas *c, const UiIcon *icons, const char *const *labels, int n)
{
	ui_blend(c, 0, HINT_Y - 18, UI_W, UI_H - HINT_Y + 18, 0x000000, 90);
	int x = MARGIN_X;
	for(int i = 0; i < n; i++)
		x += ui_hint(c, x, HINT_Y, icons[i], labels[i]) + 36;
	int vw = ui_text_width(&font_small, "v" APP_VERSION);
	if(x + vw < UI_W - MARGIN_X)
		ui_text(c, &font_small, UI_W - MARGIN_X - vw, HINT_Y + 6, "v" APP_VERSION, COL_MISSING);
}

static void status_text(const Menu *m, const MenuHost *h, char *out, size_t size, uint32_t *dot)
{
	const ConfigHost *ch = h->cfg_index >= 0 ? &m->cfg->hosts[h->cfg_index] : NULL;
	bool psn_ready = ch && config_has_psn(m->cfg) && (ch->use_psn || ch->have_duid);
	const char *state;
	if(h->discovered)
	{
		state = h->standby ? "Rest mode" : "Ready (LAN)";
		*dot = h->standby ? COL_STANDBY : COL_READY;
	}
	else if(psn_ready)
	{
		state = "PSN (Internet)";
		*dot = COL_ACCENT;
	}
	else
	{
		state = "Not found";
		*dot = COL_MISSING;
	}
	const char *reg = host_registered(m, h) ? "Registered" : psn_ready ? "PSN auto-auth" : "Not registered";
	const char *addr = h->addr[0] ? h->addr : (psn_ready ? "Remote via PSN" : "No IP address");
	snprintf(out, size, "%s   |   %s   |   %s   |   %s", h->ps5 ? "PS5" : "PS4", addr, state, reg);
}

static void draw_hosts(Menu *m, Canvas *c, bool hints)
{
	draw_header(m, c, "Remote Play");

	if(m->host_count == 0)
	{
		ui_text(c, &font_bold, MARGIN_X, LIST_Y + 20, "No consoles yet", COL_TEXT);
		ui_text(c, &font_normal, MARGIN_X, LIST_Y + 66, "Turn on your PS4 or PS5 on this network and it will appear here,", COL_DIM);
		ui_text(c, &font_normal, MARGIN_X, LIST_Y + 100, "or press the square button to add it by IP address.", COL_DIM);
	}

	int first = 0;
	if(m->sel >= LIST_ROWS)
		first = m->sel - LIST_ROWS + 1;
	for(int r = 0; r < LIST_ROWS && first + r < m->host_count; r++)
	{
		int i = first + r;
		const MenuHost *h = &m->hosts[i];
		int y = LIST_Y + r * ROW_H;
		if(i == m->sel)
		{
			ui_round_rect(c, MARGIN_X - 24, y, UI_W - 2 * MARGIN_X + 48, ROW_H - 8, 10, 0xffffff, 34);
			ui_round_rect(c, MARGIN_X - 24, y + 14, 5, ROW_H - 36, 2, COL_ACCENT, 255);
		}
		char line2[160];
		uint32_t dot;
		status_text(m, h, line2, sizeof(line2), &dot);
		ui_disc(c, MARGIN_X + 12, y + 26, 8, dot, 255);
		ui_text_fit(c, &font_bold, MARGIN_X + 36, y + 10, h->name, COL_TEXT, UI_W - 2 * MARGIN_X - 40);
		ui_text_fit(c, &font_small, MARGIN_X + 36, y + 44, line2, COL_DIM, UI_W - 2 * MARGIN_X - 40);
	}
	if(first > 0)
		ui_text(c, &font_small, UI_W - MARGIN_X - 20, LIST_Y - 26, "...", COL_DIM);
	if(first + LIST_ROWS < m->host_count)
		ui_text(c, &font_small, UI_W - MARGIN_X - 20, LIST_Y + LIST_ROWS * ROW_H - 10, "...", COL_DIM);

	if(m->note[0])
		ui_text_fit(c, &font_small, MARGIN_X, HINT_Y - 58, m->note, COL_WARN, UI_W - 2 * MARGIN_X);

	if(!hints)
		return;
	const char *cross = "Connect";
	if(m->host_count > 0 && !host_connectable(m, &m->hosts[m->sel]))
		cross = "Register";
	if(m->host_count > 0)
	{
		UiIcon icons[] = { UI_ICON_CROSS, UI_ICON_TRIANGLE, UI_ICON_SQUARE, UI_ICON_START, UI_ICON_CIRCLE };
		const char *labels[] = { cross, "Options", "Add by IP", "Settings", "Quit" };
		draw_hints(c, icons, labels, 5);
	}
	else
	{
		UiIcon icons[] = { UI_ICON_SQUARE, UI_ICON_START, UI_ICON_CIRCLE };
		const char *labels[] = { "Add by IP", "Settings", "Quit" };
		draw_hints(c, icons, labels, 3);
	}
}

static const char *opt_label(int opt, bool registered, bool ps5, bool use_psn)
{
	switch(opt)
	{
		case OPT_TYPE: return ps5 ? "Console type: PS5" : "Console type: PS4";
		case OPT_PSN_MODE: return use_psn ? "Connection: PSN (Internet)" : "Connection: Auto (LAN / PSN)";
		case OPT_CONNECT: return "Connect";
		case OPT_REGISTER: return registered ? "Register again with PIN" : "Register with PIN";
		case OPT_SET_IP: return "Set IP address";
		case OPT_REMOVE: return "Remove from list";
	}
	return "";
}

static void draw_options(Menu *m, Canvas *c)
{
	draw_hosts(m, c, false);
	ui_blend(c, 0, 0, UI_W, UI_H, 0x000000, 120);
	int w = 540, h = 96 + m->opt_count * 56;
	int x = UI_W - MARGIN_X - w, y = 140;
	ui_round_rect(c, x, y, w, h, 14, 0x1a2a48, 245);
	ui_round_rect(c, x, y, w, 4, 2, COL_ACCENT, 255);
	const MenuHost *mh = &m->hosts[m->sel];
	bool use_psn = mh->cfg_index >= 0 && m->cfg->hosts[mh->cfg_index].use_psn;
	ui_text_fit(c, &font_bold, x + 28, y + 24, mh->name, COL_TEXT, w - 56);
	for(int i = 0; i < m->opt_count; i++)
	{
		int iy = y + 76 + i * 56;
		if(i == m->opt_sel)
			ui_round_rect(c, x + 14, iy - 8, w - 28, 48, 8, 0xffffff, 40);
		ui_text(c, &font_normal, x + 30, iy, opt_label(m->opt_items[i], host_registered(m, mh), mh->ps5, use_psn), COL_TEXT);
	}
	UiIcon icons[] = { UI_ICON_UPDOWN, UI_ICON_CROSS, UI_ICON_CIRCLE };
	const char *labels[] = { "Choose", "Select", "Back" };
	draw_hints(c, icons, labels, 3);
}

static void setting_value(const Menu *m, int item, char *out, size_t size)
{
	const Config *cfg = m->cfg;
	switch(item)
	{
		case SET_RESOLUTION: snprintf(out, size, "%dp", cfg->resolution); break;
		case SET_FPS: snprintf(out, size, "%d fps", cfg->fps); break;
		case SET_BITRATE:
			if(cfg->bitrate)
				snprintf(out, size, "%d.%d Mbit/s", cfg->bitrate / 1000, cfg->bitrate % 1000 / 100);
			else
				snprintf(out, size, "Automatic");
			break;
		case SET_PACING:
		{
			static const char *pacing_names[] = { "Lowest delay", "Balanced", "Smoothest" };
			snprintf(out, size, "%s", pacing_names[cfg->pacing >= 0 && cfg->pacing <= 2 ? cfg->pacing : 1]);
			break;
		}
		case SET_LOW_LATENCY: snprintf(out, size, "%s", cfg->low_latency ? "On" : "Off"); break;
		case SET_VIBRATION: snprintf(out, size, "%s", cfg->vibration ? "On" : "Off"); break;
		case SET_PORT_GUESSING: snprintf(out, size, "%s", cfg->port_guessing ? "On" : "Off"); break;
		case SET_DUMP: snprintf(out, size, "%s", cfg->dump_stream ? "On" : "Off"); break;
		case SET_VERBOSE: snprintf(out, size, "%s", cfg->verbose_log ? "On" : "Off"); break;
		case SET_ACCOUNT:
			snprintf(out, size, "%s", cfg->have_account_id ? "Set" : "Not set");
			break;
		case SET_PSN_DISCOVER:
			snprintf(out, size, "%s", config_has_psn(cfg) ? "Ready" : "No token");
			break;
		case SET_IMPORT: out[0] = '\0'; break;
		default: out[0] = '\0';
	}
}

static void draw_settings(Menu *m, Canvas *c)
{
	draw_header(m, c, "Settings");
	static const char *names[SET_COUNT] = {
		"Resolution", "Frame rate", "Bitrate", "Video pacing", "Low-latency mode", "Vibration",
		"NAT port guessing (PSN)", "Save video stream (debug)", "Detailed log (debug)",
		"PSN account ID", "Find PS5 consoles via PSN", "Import chiaki.cfg from USB"
	};
	for(int i = 0; i < SET_COUNT; i++)
	{
		int y = LIST_Y - 14 + i * 39;
		if(i == m->set_sel)
		{
			ui_round_rect(c, MARGIN_X - 24, y - 3, UI_W - 2 * MARGIN_X + 48, 36, 8, 0xffffff, 34);
			ui_round_rect(c, MARGIN_X - 24, y + 4, 5, 22, 2, COL_ACCENT, 255);
		}
		ui_text(c, &font_normal, MARGIN_X, y + 3, names[i], COL_TEXT);
		char val[48];
		setting_value(m, i, val, sizeof(val));
		int vw = ui_text_width(&font_bold, val);
		int vx = UI_W - MARGIN_X - 40 - vw;
		ui_text(c, &font_bold, vx, y + 3, val, i == m->set_sel ? COL_ACCENT : COL_TEXT);
		bool is_action = (i == SET_ACCOUNT || i == SET_PSN_DISCOVER || i == SET_IMPORT);
		if(i == m->set_sel && !is_action)
		{
			ui_text(c, &font_bold, vx - 30, y + 3, "<", COL_DIM);
			ui_text(c, &font_bold, UI_W - MARGIN_X - 26, y + 3, ">", COL_DIM);
		}
	}
	bool action_row = m->set_sel == SET_ACCOUNT || m->set_sel == SET_PSN_DISCOVER || m->set_sel == SET_IMPORT;
	UiIcon icons[] = { UI_ICON_UPDOWN, action_row ? UI_ICON_CROSS : UI_ICON_LEFTRIGHT, UI_ICON_CIRCLE };
	const char *labels[] = {
		"Choose",
		m->set_sel == SET_ACCOUNT ? "Enter" : m->set_sel == SET_PSN_DISCOVER ? "Search" : m->set_sel == SET_IMPORT ? "Import" : "Change",
		"Back"
	};
	draw_hints(c, icons, labels, 3);
}

static void draw_number(Menu *m, Canvas *c)
{
	bool pin = m->num.purpose == MENU_NUM_PIN;
	draw_header(m, c, pin ? "Register" : "IP address");
	const char *line1, *line2;
	bool target_ps5 = m->num.host >= 0 && m->num.host < m->host_count && m->hosts[m->num.host].ps5;
	if(pin)
	{
		line1 = target_ps5 ? "On the PS5: Settings > System > Remote Play > Link Device."
			: "On the PS4: Settings > Remote Play Connection Settings > Add Device.";
		line2 = "Enter the 8-digit number it shows:";
	}
	else
	{
		line1 = "Enter the console's IP address.";
		line2 = target_ps5 ? "PS5: Settings > Network > Connection Status > View Connection Status"
			: "PS4: Settings > Network > View Connection Status > IP Address";
	}
	ui_text(c, &font_normal, MARGIN_X, LIST_Y + 10, line1, COL_TEXT);
	ui_text(c, &font_normal, MARGIN_X, LIST_Y + 50, line2, COL_DIM);
	if(pin && m->num.host >= 0 && m->num.host < m->host_count)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "Console: %s (%s)", m->hosts[m->num.host].name, m->hosts[m->num.host].addr);
		ui_text_fit(c, &font_small, MARGIN_X, LIST_Y + 94, buf, COL_DIM, UI_W - 2 * MARGIN_X);
	}

	const int box_w = 58, box_h = 82, gap = 12, dot_w = pin ? 0 : 26;
	int total = m->num.len * box_w + (m->num.len - 1) * gap + (pin ? 0 : 3 * dot_w);
	int x = (UI_W - total) / 2;
	int y = 340;
	for(int i = 0; i < m->num.len; i++)
	{
		bool cur = i == m->num.cursor;
		ui_round_rect(c, x, y, box_w, box_h, 10, cur ? 0x2f5fa8 : 0xffffff, cur ? 255 : 30);
		char d[2] = { m->num.digits[i], '\0' };
		int dw = ui_text_width(&font_title, d);
		ui_text(c, &font_title, x + (box_w - dw) / 2, y + 18, d, COL_TEXT);
		if(cur)
		{
			ui_text(c, &font_small, x + box_w / 2 - 6, y - 30, "+", COL_DIM);
			ui_text(c, &font_small, x + box_w / 2 - 4, y + box_h + 6, "-", COL_DIM);
		}
		x += box_w + gap;
		if(!pin && (i == 2 || i == 5 || i == 8))
		{
			ui_text(c, &font_title, x - gap / 2 + 2, y + 18, ".", COL_TEXT);
			x += dot_w;
		}
	}
	if(m->note[0])
		ui_text_fit(c, &font_small, MARGIN_X, HINT_Y - 58, m->note, COL_WARN, UI_W - 2 * MARGIN_X);

	UiIcon icons[] = { UI_ICON_UPDOWN, UI_ICON_LEFTRIGHT, UI_ICON_CROSS, UI_ICON_CIRCLE };
	const char *labels[] = { "Change digit", "Move", pin ? "Register" : "Save", "Cancel" };
	draw_hints(c, icons, labels, 4);
}

void menu_draw(Menu *m, Canvas *c)
{
	draw_background(c);
	switch(m->screen)
	{
		case MENU_SCREEN_HOSTS: draw_hosts(m, c, true); break;
		case MENU_SCREEN_OPTIONS: draw_options(m, c); break;
		case MENU_SCREEN_SETTINGS: draw_settings(m, c); break;
		case MENU_SCREEN_NUMBER: draw_number(m, c); break;
	}
	m->dirty = false;
}
