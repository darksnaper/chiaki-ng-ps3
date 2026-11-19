// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal miniupnpc stub for the PS3 port: UPnP is skipped so holepunch.c
// proceeds straight to STUN + UDP hole punching without waiting 7 seconds.
#ifndef CHIAKI_PS3_MINIUPNPC_H
#define CHIAKI_PS3_MINIUPNPC_H

#include <stddef.h>

#define MINIUPNPC_API_VERSION 17
#define UPNPDISCOVER_SUCCESS 0

struct UPNPDev
{
	struct UPNPDev *pNext;
	char *descURL;
	char *st;
};

struct UPNPUrls
{
	char *controlURL;
};

struct IGDdatas_service
{
	char servicetype[128];
};

struct IGDdatas
{
	struct IGDdatas_service first;
};

static inline struct UPNPDev *upnpDiscover(int delay, const char *multicastif,
	const char *minissdpdsock, int localport, int ipv6, unsigned char ttl, int *error)
{
	(void)delay; (void)multicastif; (void)minissdpdsock;
	(void)localport; (void)ipv6; (void)ttl;
	if(error)
		*error = -1;
	return NULL;
}

static inline int UPNP_GetValidIGD(struct UPNPDev *devlist, struct UPNPUrls *urls,
	struct IGDdatas *data, char *lanaddr, int lanaddrlen)
{
	(void)devlist; (void)urls; (void)data; (void)lanaddr; (void)lanaddrlen;
	return 0;
}

static inline void freeUPNPDevlist(struct UPNPDev *devlist)
{
	(void)devlist;
}

#endif
