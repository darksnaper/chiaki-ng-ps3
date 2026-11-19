// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PSN OAuth2 token refresh, cloud console discovery, and NAT hole punching for PS3.
#ifndef CHIAKI_PS3_PSN_H
#define CHIAKI_PS3_PSN_H

#include "config.h"
#include <chiaki/remote/holepunch.h>
#include <stdbool.h>
#include <stddef.h>

// Refreshes cfg->psn_auth_token using cfg->psn_refresh_token and saves chiaki.cfg.
bool psn_refresh_token(Config *cfg, char *err_buf, size_t err_size);

// Ensures cfg->psn_auth_token is valid (refreshes if psn_refresh_token is present).
bool psn_ensure_token(Config *cfg, char *err_buf, size_t err_size);

// Queries PSN for PS5 consoles linked to the account, updates cfg->hosts with their
// DUIDs, and saves chiaki.cfg. Returns number of PS5 consoles found, or -1 on error.
int psn_discover_consoles(Config *cfg, char *err_buf, size_t err_size);

// Connects to a remote PS4/PS5 over the Internet via PSN signaling + UDP hole punching
// while keeping the PS3 display and status dialog alive.
// Returns a ready ChiakiHolepunchSession (CTRL hole punched) on success, or NULL on error.
ChiakiHolepunchSession psn_connect_console(Config *cfg, ConfigHost *host, char *err_buf, size_t err_size);

#endif
