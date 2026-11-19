// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal libcurl compatibility header for the PS3 port (HTTPS + WebSocket over mbedTLS).
#ifndef CHIAKI_PS3_CURL_H
#define CHIAKI_PS3_CURL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_curl_easy CURL;
typedef struct ps3_curl_share CURLSH;
typedef int curl_socket_t;
typedef int64_t curl_off_t;

#define CURL_SOCKET_BAD (-1)

typedef enum
{
	CURLE_OK = 0,
	CURLE_UNSUPPORTED_PROTOCOL = 1,
	CURLE_URL_MALFORMAT = 3,
	CURLE_COULDNT_RESOLVE_HOST = 6,
	CURLE_COULDNT_CONNECT = 7,
	CURLE_HTTP_RETURNED_ERROR = 22,
	CURLE_WRITE_ERROR = 23,
	CURLE_OUT_OF_MEMORY = 27,
	CURLE_OPERATION_TIMEDOUT = 28,
	CURLE_SSL_CONNECT_ERROR = 35,
	CURLE_ABORTED_BY_CALLBACK = 42,
	CURLE_UNKNOWN_OPTION = 48,
	CURLE_GOT_NOTHING = 52,
	CURLE_SEND_ERROR = 55,
	CURLE_RECV_ERROR = 56,
	CURLE_SSL_CERTPROBLEM = 58,
	CURLE_PEER_FAILED_VERIFICATION = 60,
	CURLE_SSL_CACERT = 60,
	CURLE_AGAIN = 81
} CURLcode;

typedef enum
{
	CURLOPT_URL = 10002,
	CURLOPT_WRITEDATA = 10001,
	CURLOPT_WRITEFUNCTION = 20011,
	CURLOPT_HTTPHEADER = 10023,
	CURLOPT_POSTFIELDS = 10015,
	CURLOPT_CUSTOMREQUEST = 10036,
	CURLOPT_TIMEOUT = 13,
	CURLOPT_TIMEOUT_MS = 155,
	CURLOPT_CONNECTTIMEOUT = 78,
	CURLOPT_FAILONERROR = 45,
	CURLOPT_NOPROGRESS = 43,
	CURLOPT_XFERINFOFUNCTION = 20219,
	CURLOPT_XFERINFODATA = 10057,
	CURLOPT_SHARE = 10100,
	CURLOPT_CONNECT_ONLY = 141,
	CURLOPT_CERTINFO = 172,
	CURLOPT_SSL_VERIFYPEER = 64,
	CURLOPT_SSL_VERIFYHOST = 81,
	CURLOPT_SSL_OPTIONS = 216,
	CURLOPT_CAINFO_BLOB = 40309,
	CURLOPT_PROXY = 10004,
	CURLOPT_IPRESOLVE = 113,
	CURLOPT_PROTOCOLS_STR = 10318,
	CURLOPT_REDIR_PROTOCOLS_STR = 10319,
	CURLOPT_OPENSOCKETFUNCTION = 20163,
	CURLOPT_OPENSOCKETDATA = 10164
} CURLoption;

typedef enum
{
	CURLINFO_RESPONSE_CODE = 0x200002,
	CURLINFO_SSL_VERIFYRESULT = 0x20000d,
	CURLINFO_ACTIVESOCKET = 0x50002c,
	CURLINFO_CAINFO = 0x10003d
} CURLINFO;

typedef enum
{
	CURLSOCKTYPE_IPCXN = 0
} curlsocktype;

struct curl_sockaddr
{
	int family;
	int socktype;
	int protocol;
	unsigned int addrlen;
	struct sockaddr addr;
};

#define CURL_IPRESOLVE_V4 1L

#define CURL_BLOB_COPY 1
struct curl_blob
{
	void *data;
	size_t len;
	unsigned int flags;
};

#define CURLSSLOPT_NO_PARTIALCHAIN (1L << 2)
#define CURLSSLOPT_SCHANNEL_AIA    0L

struct curl_slist
{
	char *data;
	struct curl_slist *next;
};

#define CURLWS_TEXT   (1 << 0)
#define CURLWS_BINARY (1 << 1)
#define CURLWS_CONT   (1 << 2)
#define CURLWS_CLOSE  (1 << 3)
#define CURLWS_PING   (1 << 4)
#define CURLWS_OFFSET (1 << 5)
#define CURLWS_PONG   (1 << 6)

struct curl_ws_frame
{
	int age;
	int flags;
	curl_off_t offset;
	curl_off_t bytesleft;
	size_t len;
};

typedef enum
{
	CURLVERSION_NOW = 10
} CURLversion;

typedef struct
{
	CURLversion age;
	const char *version;
	unsigned int version_num;
	const char *host;
	int features;
	const char *ssl_version;
} curl_version_info_data;

typedef size_t (*curl_write_callback)(void *ptr, size_t size, size_t nmemb, void *userdata);
typedef int (*curl_xferinfo_callback)(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *str);
void curl_slist_free_all(struct curl_slist *list);

CURLSH *curl_share_init(void);
void curl_share_cleanup(CURLSH *share);

const curl_version_info_data *curl_version_info(CURLversion ver);
char *curl_version(void);

CURLcode curl_ws_send(CURL *curl, const void *buffer, size_t buflen, size_t *sent, curl_off_t fragsize, unsigned int sendflags);
CURLcode curl_ws_recv(CURL *curl, void *buffer, size_t buflen, size_t *recv, const struct curl_ws_frame **meta);

#ifdef __cplusplus
}
#endif

#endif
