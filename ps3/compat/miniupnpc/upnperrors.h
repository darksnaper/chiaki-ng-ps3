// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal miniupnpc errors stub for the PS3 port.
#ifndef CHIAKI_PS3_UPNPERRORS_H
#define CHIAKI_PS3_UPNPERRORS_H

static inline const char *strupnperror(int err)
{
	(void)err;
	return "UPnP unavailable on PS3";
}

#endif
