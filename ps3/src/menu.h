// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Start menu: console list, console options, settings, number entry.
// Platform independent (state, input handling, drawing); the PS3 side feeds
// it discovery results and button events and carries out its actions.
#ifndef CHIAKI_PS3_MENU_H
#define CHIAKI_PS3_MENU_H

#include "config.h"
#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#define MENU_HOSTS_MAX 16

typedef struct
{
	char name[64];
	char addr[64];
	char id[32];
	bool standby;
	bool ps5;
} DiscoveredHost;

typedef struct
{
	char name[64];
	char addr[64];
	char id[32];
	bool discovered;  // answered discovery recently
	bool standby;     // in rest mode (only meaningful when discovered)
	bool ps5;
	int cfg_index;    // index in Config.hosts, -1 if not saved
} MenuHost;

typedef enum
{
	MENU_SCREEN_HOSTS,
	MENU_SCREEN_OPTIONS,
	MENU_SCREEN_SETTINGS,
	MENU_SCREEN_NUMBER
} MenuScreen;

typedef enum
{
	MENU_NUM_PIN,
	MENU_NUM_IP_ADD,
	MENU_NUM_IP_EDIT
} MenuNumberPurpose;

typedef enum
{
	MENU_ACT_NONE,
	MENU_ACT_CONNECT,   // host = config index
	MENU_ACT_REGISTER,  // host = menu host index, pin
	MENU_ACT_SAVE,      // config changed
	MENU_ACT_QUIT,
	MENU_ACT_MESSAGE,   // show message (text)
	MENU_ACT_ACCOUNT,   // ask for the PSN account id; host = menu host index to register afterwards, or -1
	MENU_ACT_IMPORT,    // import chiaki.cfg from a USB stick
	MENU_ACT_PSN_DISCOVER // fetch PS5 consoles from PSN account
} MenuActionType;

typedef struct
{
	MenuActionType type;
	int host;
	uint32_t pin;
	char text[512];
} MenuAction;

typedef struct
{
	Config *cfg;
	MenuScreen screen;

	MenuHost hosts[MENU_HOSTS_MAX];
	int host_count;
	DiscoveredHost discovered[MENU_HOSTS_MAX];
	int discovered_count;
	int sel;

	int opt_items[6];
	int opt_count;
	int opt_sel;

	int set_sel;
	bool settings_dirty;

	struct
	{
		MenuNumberPurpose purpose;
		int host;          // menu host index for PIN / IP edit
		char digits[16];   // '0'..'9'
		int len;
		int cursor;
	} num;

	char note[160];        // one-line message under the list
	char ps3_ip[32];
	bool dirty;            // needs redraw
	bool cfg_changed;      // the menu changed the config by itself (e.g. console type from discovery); caller saves
} Menu;

void menu_init(Menu *m, Config *cfg);
// Replaces the discovery results and rebuilds the list (config hosts first).
void menu_set_discovered(Menu *m, const DiscoveredHost *list, int count);
// Call after the config changed outside the menu (e.g. registration saved).
void menu_rebuild(Menu *m);
void menu_set_note(Menu *m, const char *note);
// Opens the PIN entry for a console in the list (after the account id was entered).
void menu_begin_register(Menu *m, int host);
MenuAction menu_handle(Menu *m, uint32_t events);
void menu_draw(Menu *m, Canvas *c);

#endif
