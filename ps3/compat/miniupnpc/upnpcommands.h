// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal miniupnpc commands stub for the PS3 port.
#ifndef CHIAKI_PS3_UPNPCOMMANDS_H
#define CHIAKI_PS3_UPNPCOMMANDS_H

#define UPNPCOMMAND_SUCCESS 0

static inline int UPNP_GetExternalIPAddress(const char *controlURL, const char *servicetype, char *extIpAdd)
{
	(void)controlURL; (void)servicetype; (void)extIpAdd;
	return -1;
}

static inline int UPNP_AddPortMapping(const char *controlURL, const char *servicetype,
	const char *extPort, const char *inPort, const char *inClient, const char *desc,
	const char *proto, const char *remoteHost, const char *leaseDuration)
{
	(void)controlURL; (void)servicetype; (void)extPort; (void)inPort;
	(void)inClient; (void)desc; (void)proto; (void)remoteHost; (void)leaseDuration;
	return -1;
}

static inline int UPNP_DeletePortMapping(const char *controlURL, const char *servicetype,
	const char *extPort, const char *proto, const char *remoteHost)
{
	(void)controlURL; (void)servicetype; (void)extPort; (void)proto; (void)remoteHost;
	return -1;
}

#endif
