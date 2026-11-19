// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Minimal libcurl-compatible HTTPS + WebSocket (wss://) client for PS3
// built directly on mbedTLS (or OpenSSL for host unit tests) + POSIX/PSL1GHT sockets.

#include <curl/curl.h>
#include <chiaki/base64.h>
#include <chiaki/random.h>
#include <chiaki/sock.h>
#include <chiaki/time.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#else
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#endif

struct ps3_curl_share
{
	int dummy;
};

struct ps3_curl_easy
{
	char *url;
	struct curl_slist *headers;
	char *postfields;
	char *custom_request;
	curl_write_callback write_cb;
	void *write_data;
	curl_xferinfo_callback xfer_cb;
	void *xfer_data;
	long timeout_ms;
	long connect_timeout_ms;
	bool fail_on_error;
	long connect_only;
	bool noprogress;

	long response_code;
	long ssl_verify_result;

	// Active connection (kept open for WebSocket / connect_only == 2)
	int sock;
	bool use_tls;
	bool connected;
#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
	SSL_CTX *ssl_ctx;
	SSL *ssl;
#else
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	bool tls_initialized;
#endif

	// WebSocket receive buffer and frame metadata
	uint8_t *ws_rx_buf;
	size_t ws_rx_len;
	size_t ws_rx_cap;
	uint8_t *ws_frag_buf;
	size_t ws_frag_len;
	size_t ws_frag_cap;
	unsigned int ws_frag_flags;
	struct curl_ws_frame ws_meta;
};

// ---------------------------------------------------------------------------
// Utility helpers

static char *ps3_strdup(const char *s)
{
	if(!s)
		return NULL;
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if(p)
		memcpy(p, s, n);
	return p;
}

static bool buf_append(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
	if(n == 0)
		return true;
	if(*len + n > *cap)
	{
		size_t ncap = *cap ? *cap * 2 : 4096;
		while(ncap < *len + n)
			ncap *= 2;
		uint8_t *nb = realloc(*buf, ncap);
		if(!nb)
			return false;
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, data, n);
	*len += n;
	return true;
}

static void buf_consume(uint8_t *buf, size_t *len, size_t n)
{
	if(n >= *len)
	{
		*len = 0;
		return;
	}
	memmove(buf, buf + n, *len - n);
	*len -= n;
}

static bool slist_has_header(const struct curl_slist *list, const char *name)
{
	size_t nlen = strlen(name);
	for(const struct curl_slist *it = list; it; it = it->next)
	{
		if(it->data && strncasecmp(it->data, name, nlen) == 0 && it->data[nlen] == ':')
			return true;
	}
	return false;
}

static bool check_aborted(CURL *c)
{
	if(!c->noprogress && c->xfer_cb)
		return c->xfer_cb(c->xfer_data, 0, 0, 0, 0) != 0;
	return false;
}

typedef struct
{
	char scheme[16];
	char host[256];
	uint16_t port;
	char path[1024];
	bool tls;
	bool ws;
} ParsedUrl;

static bool parse_url(const char *url, ParsedUrl *out)
{
	memset(out, 0, sizeof(*out));
	if(!url)
		return false;
	const char *sep = strstr(url, "://");
	if(!sep)
		return false;
	size_t slen = (size_t)(sep - url);
	if(slen == 0 || slen >= sizeof(out->scheme))
		return false;
	memcpy(out->scheme, url, slen);
	out->scheme[slen] = '\0';

	if(strcasecmp(out->scheme, "https") == 0)
	{
		out->tls = true;
		out->ws = false;
		out->port = 443;
	}
	else if(strcasecmp(out->scheme, "http") == 0)
	{
		out->tls = false;
		out->ws = false;
		out->port = 80;
	}
	else if(strcasecmp(out->scheme, "wss") == 0)
	{
		out->tls = true;
		out->ws = true;
		out->port = 443;
	}
	else if(strcasecmp(out->scheme, "ws") == 0)
	{
		out->tls = false;
		out->ws = true;
		out->port = 80;
	}
	else
	{
		return false;
	}

	const char *hp = sep + 3;
	const char *slash = strchr(hp, '/');
	const char *qmark = strchr(hp, '?');
	const char *path_start = NULL;
	if(slash && (!qmark || slash < qmark))
		path_start = slash;
	else if(qmark)
		path_start = qmark;

	size_t hplen = path_start ? (size_t)(path_start - hp) : strlen(hp);
	if(hplen == 0 || hplen >= sizeof(out->host))
		return false;

	char hostport[256];
	memcpy(hostport, hp, hplen);
	hostport[hplen] = '\0';

	char *colon = strrchr(hostport, ':');
	if(colon)
	{
		*colon = '\0';
		unsigned long p = strtoul(colon + 1, NULL, 10);
		if(p == 0 || p > 65535)
			return false;
		out->port = (uint16_t)p;
	}
	snprintf(out->host, sizeof(out->host), "%s", hostport);

	if(!path_start)
		snprintf(out->path, sizeof(out->path), "/");
	else if(*path_start == '?')
		snprintf(out->path, sizeof(out->path), "/%s", path_start);
	else
		snprintf(out->path, sizeof(out->path), "%s", path_start);

	return true;
}

// ---------------------------------------------------------------------------
// Low-level socket + TLS transport

static void transport_close(CURL *c)
{
#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
	if(c->ssl)
	{
		SSL_shutdown(c->ssl);
		SSL_free(c->ssl);
		c->ssl = NULL;
	}
	if(c->ssl_ctx)
	{
		SSL_CTX_free(c->ssl_ctx);
		c->ssl_ctx = NULL;
	}
#else
	if(c->tls_initialized)
	{
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_ctr_drbg_free(&c->ctr_drbg);
		mbedtls_entropy_free(&c->entropy);
		c->tls_initialized = false;
	}
#endif
	if(c->sock >= 0)
	{
		CHIAKI_SOCKET_CLOSE(c->sock);
		c->sock = -1;
	}
	c->connected = false;
	c->ws_rx_len = 0;
	c->ws_frag_len = 0;
	c->ws_frag_flags = 0;
}

static int poll_socket(CURL *c, short events, uint64_t deadline_ms)
{
	while(true)
	{
		if(check_aborted(c))
			return -2; // aborted
		int wait_ms = 200;
		if(deadline_ms)
		{
			uint64_t now = chiaki_time_now_monotonic_ms();
			if(now >= deadline_ms)
				return 0; // timeout
			uint64_t rem = deadline_ms - now;
			if(rem < (uint64_t)wait_ms)
				wait_ms = (int)rem;
		}
		struct pollfd pfd;
		pfd.fd = c->sock;
		pfd.events = events;
		pfd.revents = 0;
		int r = poll(&pfd, 1, wait_ms);
		if(r > 0)
			return 1;
		if(r < 0 && errno != EINTR)
			return -1;
	}
}

#ifndef CHIAKI_PS3_CURL_HOST_OPENSSL
static int mbed_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = send(fd, (const char *)buf, len, 0);
	if(r < 0)
	{
		int e = errno;
		if(e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return -1;
	}
	return (int)r;
}

static int mbed_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = recv(fd, (char *)buf, len, 0);
	if(r < 0)
	{
		int e = errno;
		if(e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return -1;
	}
	if(r == 0)
		return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
	return (int)r;
}
#endif

static CURLcode transport_connect(CURL *c, const ParsedUrl *u, uint64_t deadline_ms)
{
	transport_close(c);
	c->use_tls = u->tls;

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%u", (unsigned)u->port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	if(getaddrinfo(u->host, port_str, &hints, &res) != 0 || !res)
		return CURLE_COULDNT_RESOLVE_HOST;

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(fd < 0)
	{
		freeaddrinfo(res);
		return CURLE_COULDNT_CONNECT;
	}
	chiaki_socket_set_nonblock(fd, true);
	c->sock = fd;

	int rc = connect(fd, res->ai_addr, res->ai_addrlen);
	int saved_errno = errno;
	freeaddrinfo(res);

	if(rc < 0)
	{
		if(saved_errno != EINPROGRESS && saved_errno != EWOULDBLOCK)
		{
			transport_close(c);
			return CURLE_COULDNT_CONNECT;
		}
		int pr = poll_socket(c, POLLOUT, deadline_ms);
		if(pr == -2)
		{
			transport_close(c);
			return CURLE_ABORTED_BY_CALLBACK;
		}
		if(pr == 0)
		{
			transport_close(c);
			return CURLE_OPERATION_TIMEDOUT;
		}
		if(pr < 0)
		{
			transport_close(c);
			return CURLE_COULDNT_CONNECT;
		}
		int so_err = 0;
		socklen_t so_len = sizeof(so_err);
		if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0 || so_err != 0)
		{
			transport_close(c);
			return CURLE_COULDNT_CONNECT;
		}
	}

	if(!c->use_tls)
	{
		c->connected = true;
		return CURLE_OK;
	}

#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
	c->ssl_ctx = SSL_CTX_new(TLS_client_method());
	if(!c->ssl_ctx)
	{
		transport_close(c);
		return CURLE_OUT_OF_MEMORY;
	}
	SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_NONE, NULL);
	c->ssl = SSL_new(c->ssl_ctx);
	if(!c->ssl)
	{
		transport_close(c);
		return CURLE_OUT_OF_MEMORY;
	}
	SSL_set_tlsext_host_name(c->ssl, u->host);
	SSL_set_fd(c->ssl, c->sock);

	while(true)
	{
		int hr = SSL_connect(c->ssl);
		if(hr == 1)
			break;
		int err = SSL_get_error(c->ssl, hr);
		if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		{
			int pr = poll_socket(c, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) { transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
			if(pr == 0)  { transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
			if(pr < 0)   { transport_close(c); return CURLE_SSL_CONNECT_ERROR; }
			continue;
		}
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}
#else
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_ctr_drbg_init(&c->ctr_drbg);
	mbedtls_entropy_init(&c->entropy);
	c->tls_initialized = true;

	const char *pers = "chiaki_ps3_tls";
	if(mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
			(const unsigned char *)pers, strlen(pers)) != 0)
	{
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}

	if(mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
			MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
	{
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}

	// PS3 has no modern system CA store; skip peer chain verification so PSN/STUN endpoints work.
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

	if(mbedtls_ssl_setup(&c->ssl, &c->conf) != 0)
	{
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}
	if(mbedtls_ssl_set_hostname(&c->ssl, u->host) != 0)
	{
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}
	mbedtls_ssl_set_bio(&c->ssl, &c->sock, mbed_bio_send, mbed_bio_recv, NULL);

	while(true)
	{
		int hr = mbedtls_ssl_handshake(&c->ssl);
		if(hr == 0)
			break;
		if(hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			int pr = poll_socket(c, hr == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) { transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
			if(pr == 0)  { transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
			if(pr < 0)   { transport_close(c); return CURLE_SSL_CONNECT_ERROR; }
			continue;
		}
		transport_close(c);
		return CURLE_SSL_CONNECT_ERROR;
	}
#endif

	c->connected = true;
	return CURLE_OK;
}

static CURLcode transport_write_all(CURL *c, const void *data, size_t len, uint64_t deadline_ms)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t off = 0;
	while(off < len)
	{
		if(check_aborted(c))
			return CURLE_ABORTED_BY_CALLBACK;
		if(!c->use_tls)
		{
			ssize_t r = send(c->sock, (const char *)(p + off), len - off, 0);
			if(r > 0)
			{
				off += (size_t)r;
				continue;
			}
			if(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			{
				int pr = poll_socket(c, POLLOUT, deadline_ms);
				if(pr == -2) return CURLE_ABORTED_BY_CALLBACK;
				if(pr == 0)  return CURLE_OPERATION_TIMEDOUT;
				if(pr < 0)   return CURLE_SEND_ERROR;
				continue;
			}
			return CURLE_SEND_ERROR;
		}
#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
		int r = SSL_write(c->ssl, p + off, (int)(len - off));
		if(r > 0)
		{
			off += (size_t)r;
			continue;
		}
		int err = SSL_get_error(c->ssl, r);
		if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		{
			int pr = poll_socket(c, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) return CURLE_ABORTED_BY_CALLBACK;
			if(pr == 0)  return CURLE_OPERATION_TIMEDOUT;
			if(pr < 0)   return CURLE_SEND_ERROR;
			continue;
		}
		return CURLE_SEND_ERROR;
#else
		int r = mbedtls_ssl_write(&c->ssl, p + off, len - off);
		if(r > 0)
		{
			off += (size_t)r;
			continue;
		}
		if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			int pr = poll_socket(c, r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) return CURLE_ABORTED_BY_CALLBACK;
			if(pr == 0)  return CURLE_OPERATION_TIMEDOUT;
			if(pr < 0)   return CURLE_SEND_ERROR;
			continue;
		}
		return CURLE_SEND_ERROR;
#endif
	}
	return CURLE_OK;
}

// Returns >0 bytes read, 0 on clean EOF, -1 on error, -2 on WANT_READ (nonblocking),
// -3 on timeout, -4 on aborted.
static int transport_read_some(CURL *c, uint8_t *buf, size_t cap, bool nonblock, uint64_t deadline_ms)
{
	while(true)
	{
		if(!nonblock && check_aborted(c))
			return -4;
		if(!c->use_tls)
		{
			ssize_t r = recv(c->sock, (char *)buf, cap, 0);
			if(r > 0)
				return (int)r;
			if(r == 0)
				return 0;
			if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			{
				if(nonblock)
					return -2;
				int pr = poll_socket(c, POLLIN, deadline_ms);
				if(pr == -2) return -4;
				if(pr == 0)  return -3;
				if(pr < 0)   return -1;
				continue;
			}
			return -1;
		}
#ifdef CHIAKI_PS3_CURL_HOST_OPENSSL
		int r = SSL_read(c->ssl, buf, (int)cap);
		if(r > 0)
			return r;
		int err = SSL_get_error(c->ssl, r);
		if(err == SSL_ERROR_ZERO_RETURN)
			return 0;
		if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		{
			if(nonblock)
				return -2;
			int pr = poll_socket(c, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) return -4;
			if(pr == 0)  return -3;
			if(pr < 0)   return -1;
			continue;
		}
		return -1;
#else
		int r = mbedtls_ssl_read(&c->ssl, buf, cap);
		if(r > 0)
			return r;
		if(r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			if(nonblock)
				return -2;
			int pr = poll_socket(c, r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, deadline_ms);
			if(pr == -2) return -4;
			if(pr == 0)  return -3;
			if(pr < 0)   return -1;
			continue;
		}
		return -1;
#endif
	}
}

// ---------------------------------------------------------------------------
// HTTP/1.1 request & response handling

static char *find_header_end(const uint8_t *buf, size_t len)
{
	if(len < 4)
		return NULL;
	for(size_t i = 0; i + 3 < len; i++)
	{
		if(buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
			return (char *)(buf + i + 4);
	}
	return NULL;
}

static bool parse_http_headers(const char *hdrs, size_t hdr_len, long *status_out,
	bool *chunked_out, int64_t *content_len_out, char *location_out, size_t loc_size)
{
	*status_out = 0;
	*chunked_out = false;
	*content_len_out = -1;
	if(location_out && loc_size)
		location_out[0] = '\0';

	// Parse status line: HTTP/1.x 200 OK
	const char *p = hdrs;
	const char *end = hdrs + hdr_len;
	const char *sp = memchr(p, ' ', (size_t)(end - p));
	if(!sp)
		return false;
	while(sp < end && *sp == ' ')
		sp++;
	*status_out = strtol(sp, NULL, 10);

	const char *line = memchr(sp, '\n', (size_t)(end - sp));
	if(!line)
		return true;
	line++;

	while(line < end)
	{
		const char *nl = memchr(line, '\n', (size_t)(end - line));
		size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);
		while(llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == '\n'))
			llen--;
		if(llen == 0)
			break;

		const char *colon = memchr(line, ':', llen);
		if(colon)
		{
			size_t klen = (size_t)(colon - line);
			const char *val = colon + 1;
			size_t vlen = llen - klen - 1;
			while(vlen > 0 && (*val == ' ' || *val == '\t'))
			{
				val++;
				vlen--;
			}
			while(vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
				vlen--;

			if(klen == 14 && strncasecmp(line, "Content-Length", 14) == 0)
			{
				char tmp[32];
				size_t cl = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
				memcpy(tmp, val, cl);
				tmp[cl] = '\0';
				*content_len_out = strtoll(tmp, NULL, 10);
			}
			else if(klen == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
			{
				for(size_t i = 0; i + 7 <= vlen; i++)
				{
					if(strncasecmp(val + i, "chunked", 7) == 0)
					{
						*chunked_out = true;
						break;
					}
				}
			}
			else if(location_out && loc_size && klen == 8 && strncasecmp(line, "Location", 8) == 0)
			{
				size_t cl = vlen < loc_size - 1 ? vlen : loc_size - 1;
				memcpy(location_out, val, cl);
				location_out[cl] = '\0';
			}
		}

		if(!nl)
			break;
		line = nl + 1;
	}
	return true;
}

static bool deliver_body(CURL *c, const void *data, size_t len)
{
	if(len == 0 || !c->write_cb)
		return true;
	size_t written = c->write_cb((void *)data, 1, len, c->write_data);
	return written == len;
}

static CURLcode perform_websocket_handshake(CURL *c, const ParsedUrl *u, uint64_t deadline_ms)
{
	uint8_t key_raw[16];
	chiaki_random_bytes_crypt(key_raw, sizeof(key_raw));
	char key_b64[32] = {0};
	if(chiaki_base64_encode(key_raw, sizeof(key_raw), key_b64, sizeof(key_b64)) != CHIAKI_ERR_SUCCESS)
		return CURLE_OUT_OF_MEMORY;

	uint8_t *req = NULL;
	size_t req_len = 0, req_cap = 0;
	char line[1280];

	int n = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n", u->path);
	if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
		goto oom;

	if(!slist_has_header(c->headers, "Host"))
	{
		if((u->tls && u->port != 443) || (!u->tls && u->port != 80))
			n = snprintf(line, sizeof(line), "Host: %s:%u\r\n", u->host, (unsigned)u->port);
		else
			n = snprintf(line, sizeof(line), "Host: %s\r\n", u->host);
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}

	n = snprintf(line, sizeof(line),
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n",
		key_b64);
	if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
		goto oom;

	for(const struct curl_slist *it = c->headers; it; it = it->next)
	{
		if(!it->data || !it->data[0])
			continue;
		if(!buf_append(&req, &req_len, &req_cap, it->data, strlen(it->data)) ||
			!buf_append(&req, &req_len, &req_cap, "\r\n", 2))
			goto oom;
	}
	if(!buf_append(&req, &req_len, &req_cap, "\r\n", 2))
		goto oom;

	CURLcode err = transport_write_all(c, req, req_len, deadline_ms);
	free(req);
	if(err != CURLE_OK)
		return err;

	// Read upgrade response headers
	uint8_t tmp[2048];
	char *hdr_end = NULL;
	while(!hdr_end)
	{
		int r = transport_read_some(c, tmp, sizeof(tmp), false, deadline_ms);
		if(r == -4) return CURLE_ABORTED_BY_CALLBACK;
		if(r == -3) return CURLE_OPERATION_TIMEDOUT;
		if(r <= 0)  return CURLE_RECV_ERROR;
		if(!buf_append(&c->ws_rx_buf, &c->ws_rx_len, &c->ws_rx_cap, tmp, (size_t)r))
			return CURLE_OUT_OF_MEMORY;
		hdr_end = find_header_end(c->ws_rx_buf, c->ws_rx_len);
		if(c->ws_rx_len > 32768 && !hdr_end)
			return CURLE_RECV_ERROR;
	}

	size_t hdr_len = (size_t)((uint8_t *)hdr_end - c->ws_rx_buf);
	bool chunked = false;
	int64_t content_len = -1;
	parse_http_headers((const char *)c->ws_rx_buf, hdr_len, &c->response_code, &chunked, &content_len, NULL, 0);
	buf_consume(c->ws_rx_buf, &c->ws_rx_len, hdr_len);

	if(c->response_code != 101)
		return c->fail_on_error ? CURLE_HTTP_RETURNED_ERROR : CURLE_RECV_ERROR;

	return CURLE_OK;

oom:
	free(req);
	return CURLE_OUT_OF_MEMORY;
}

static CURLcode perform_http_once(CURL *c, const ParsedUrl *u, uint64_t deadline_ms, char *redirect_url, size_t redirect_size)
{
	if(redirect_url && redirect_size)
		redirect_url[0] = '\0';

	CURLcode err = transport_connect(c, u, deadline_ms);
	if(err != CURLE_OK)
		return err;

	if(c->connect_only == 2L || u->ws)
		return perform_websocket_handshake(c, u, deadline_ms);

	const char *method = c->custom_request ? c->custom_request : (c->postfields ? "POST" : "GET");
	size_t body_len = c->postfields ? strlen(c->postfields) : 0;

	uint8_t *req = NULL;
	size_t req_len = 0, req_cap = 0;
	char line[1280];

	int n = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, u->path);
	if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
		goto oom;

	if(!slist_has_header(c->headers, "Host"))
	{
		if((u->tls && u->port != 443) || (!u->tls && u->port != 80))
			n = snprintf(line, sizeof(line), "Host: %s:%u\r\n", u->host, (unsigned)u->port);
		else
			n = snprintf(line, sizeof(line), "Host: %s\r\n", u->host);
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}
	if(!slist_has_header(c->headers, "User-Agent"))
	{
		n = snprintf(line, sizeof(line), "User-Agent: chiaki-ng-ps3/1.0\r\n");
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}
	if(!slist_has_header(c->headers, "Accept"))
	{
		n = snprintf(line, sizeof(line), "Accept: */*\r\n");
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}
	if(!slist_has_header(c->headers, "Connection"))
	{
		n = snprintf(line, sizeof(line), "Connection: close\r\n");
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}
	if(c->postfields && !slist_has_header(c->headers, "Content-Length"))
	{
		n = snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
		if(!buf_append(&req, &req_len, &req_cap, line, (size_t)n))
			goto oom;
	}

	for(const struct curl_slist *it = c->headers; it; it = it->next)
	{
		if(!it->data || !it->data[0])
			continue;
		if(!buf_append(&req, &req_len, &req_cap, it->data, strlen(it->data)) ||
			!buf_append(&req, &req_len, &req_cap, "\r\n", 2))
			goto oom;
	}
	if(!buf_append(&req, &req_len, &req_cap, "\r\n", 2))
		goto oom;
	if(body_len > 0 && !buf_append(&req, &req_len, &req_cap, c->postfields, body_len))
		goto oom;

	err = transport_write_all(c, req, req_len, deadline_ms);
	free(req);
	if(err != CURLE_OK)
	{
		transport_close(c);
		return err;
	}

	// Read response headers
	uint8_t *rx = NULL;
	size_t rx_len = 0, rx_cap = 0;
	uint8_t tmp[4096];
	char *hdr_end = NULL;
	while(!hdr_end)
	{
		int r = transport_read_some(c, tmp, sizeof(tmp), false, deadline_ms);
		if(r == -4) { free(rx); transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
		if(r == -3) { free(rx); transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
		if(r < 0)   { free(rx); transport_close(c); return CURLE_RECV_ERROR; }
		if(r == 0)  { free(rx); transport_close(c); return CURLE_GOT_NOTHING; }
		if(!buf_append(&rx, &rx_len, &rx_cap, tmp, (size_t)r))
		{
			free(rx);
			transport_close(c);
			return CURLE_OUT_OF_MEMORY;
		}
		hdr_end = find_header_end(rx, rx_len);
		if(rx_len > 65536 && !hdr_end)
		{
			free(rx);
			transport_close(c);
			return CURLE_RECV_ERROR;
		}
	}

	size_t hdr_len = (size_t)((uint8_t *)hdr_end - rx);
	bool chunked = false;
	int64_t content_len = -1;
	parse_http_headers((const char *)rx, hdr_len, &c->response_code, &chunked, &content_len, redirect_url, redirect_size);
	buf_consume(rx, &rx_len, hdr_len);

	// Check for HTTP redirect on GET
	if((c->response_code == 301 || c->response_code == 302 || c->response_code == 307 || c->response_code == 308)
		&& redirect_url && redirect_url[0] && strcmp(method, "GET") == 0)
	{
		free(rx);
		transport_close(c);
		return CURLE_OK;
	}

	bool is_no_body = (c->response_code == 204 || c->response_code == 304 || strcasecmp(method, "HEAD") == 0);
	bool deliver_ok = true;

	if(!is_no_body)
	{
		if(chunked)
		{
			bool done = false;
			while(!done)
			{
				// Find chunk size line
				char *crlf = NULL;
				while(!(crlf = (char *)memchr(rx, '\n', rx_len)))
				{
					int r = transport_read_some(c, tmp, sizeof(tmp), false, deadline_ms);
					if(r == -4) { free(rx); transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
					if(r == -3) { free(rx); transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
					if(r <= 0)  { done = true; break; }
					if(!buf_append(&rx, &rx_len, &rx_cap, tmp, (size_t)r))
					{
						free(rx);
						transport_close(c);
						return CURLE_OUT_OF_MEMORY;
					}
				}
				if(done || !crlf)
					break;
				size_t line_len = (size_t)((uint8_t *)crlf - rx) + 1;
				char size_line[64];
				size_t sl = line_len < sizeof(size_line) - 1 ? line_len : sizeof(size_line) - 1;
				memcpy(size_line, rx, sl);
				size_line[sl] = '\0';
				buf_consume(rx, &rx_len, line_len);

				unsigned long chunk_size = strtoul(size_line, NULL, 16);
				if(chunk_size == 0)
				{
					done = true;
					break;
				}
				// Ensure chunk_size + 2 (CRLF) bytes are in rx
				size_t need = (size_t)chunk_size + 2;
				while(rx_len < need)
				{
					int r = transport_read_some(c, tmp, sizeof(tmp), false, deadline_ms);
					if(r == -4) { free(rx); transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
					if(r == -3) { free(rx); transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
					if(r <= 0)  { done = true; break; }
					if(!buf_append(&rx, &rx_len, &rx_cap, tmp, (size_t)r))
					{
						free(rx);
						transport_close(c);
						return CURLE_OUT_OF_MEMORY;
					}
				}
				size_t avail = rx_len < (size_t)chunk_size ? rx_len : (size_t)chunk_size;
				if(!deliver_body(c, rx, avail))
				{
					deliver_ok = false;
					break;
				}
				buf_consume(rx, &rx_len, rx_len >= need ? need : rx_len);
			}
		}
		else if(content_len >= 0)
		{
			uint64_t remaining = (uint64_t)content_len;
			if(rx_len > 0)
			{
				size_t take = rx_len > remaining ? (size_t)remaining : rx_len;
				if(!deliver_body(c, rx, take))
					deliver_ok = false;
				remaining -= take;
			}
			while(deliver_ok && remaining > 0)
			{
				size_t want = remaining > sizeof(tmp) ? sizeof(tmp) : (size_t)remaining;
				int r = transport_read_some(c, tmp, want, false, deadline_ms);
				if(r == -4) { free(rx); transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
				if(r == -3) { free(rx); transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
				if(r <= 0)  break;
				if(!deliver_body(c, tmp, (size_t)r))
				{
					deliver_ok = false;
					break;
				}
				remaining -= (uint64_t)r;
			}
		}
		else
		{
			if(rx_len > 0 && !deliver_body(c, rx, rx_len))
				deliver_ok = false;
			while(deliver_ok)
			{
				int r = transport_read_some(c, tmp, sizeof(tmp), false, deadline_ms);
				if(r == -4) { free(rx); transport_close(c); return CURLE_ABORTED_BY_CALLBACK; }
				if(r == -3) { free(rx); transport_close(c); return CURLE_OPERATION_TIMEDOUT; }
				if(r <= 0)  break;
				if(!deliver_body(c, tmp, (size_t)r))
				{
					deliver_ok = false;
					break;
				}
			}
		}
	}

	free(rx);
	transport_close(c);

	if(!deliver_ok)
		return CURLE_WRITE_ERROR;
	if(c->fail_on_error && c->response_code >= 400)
		return CURLE_HTTP_RETURNED_ERROR;
	return CURLE_OK;

oom:
	free(req);
	transport_close(c);
	return CURLE_OUT_OF_MEMORY;
}

// ---------------------------------------------------------------------------
// Public libcurl-compatible API

CURL *curl_easy_init(void)
{
	CURL *c = calloc(1, sizeof(CURL));
	if(!c)
		return NULL;
	c->sock = -1;
	c->noprogress = true;
	c->timeout_ms = 30000;
	c->connect_timeout_ms = 15000;
	return c;
}

void curl_easy_cleanup(CURL *curl)
{
	if(!curl)
		return;
	transport_close(curl);
	free(curl->url);
	free(curl->postfields);
	free(curl->custom_request);
	free(curl->ws_rx_buf);
	free(curl->ws_frag_buf);
	free(curl);
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
	if(!curl)
		return CURLE_UNKNOWN_OPTION;
	va_list ap;
	va_start(ap, option);
	CURLcode res = CURLE_OK;

	switch(option)
	{
		case CURLOPT_URL:
		{
			const char *s = va_arg(ap, const char *);
			free(curl->url);
			curl->url = ps3_strdup(s);
			break;
		}
		case CURLOPT_HTTPHEADER:
			curl->headers = va_arg(ap, struct curl_slist *);
			break;
		case CURLOPT_POSTFIELDS:
		{
			const char *s = va_arg(ap, const char *);
			free(curl->postfields);
			curl->postfields = ps3_strdup(s);
			break;
		}
		case CURLOPT_CUSTOMREQUEST:
		{
			const char *s = va_arg(ap, const char *);
			free(curl->custom_request);
			curl->custom_request = ps3_strdup(s);
			break;
		}
		case CURLOPT_WRITEFUNCTION:
			curl->write_cb = va_arg(ap, curl_write_callback);
			break;
		case CURLOPT_WRITEDATA:
			curl->write_data = va_arg(ap, void *);
			break;
		case CURLOPT_XFERINFOFUNCTION:
			curl->xfer_cb = va_arg(ap, curl_xferinfo_callback);
			break;
		case CURLOPT_XFERINFODATA:
			curl->xfer_data = va_arg(ap, void *);
			break;
		case CURLOPT_NOPROGRESS:
			curl->noprogress = va_arg(ap, long) != 0;
			break;
		case CURLOPT_TIMEOUT:
		{
			long sec = va_arg(ap, long);
			curl->timeout_ms = sec > 0 ? sec * 1000L : 0;
			break;
		}
		case CURLOPT_TIMEOUT_MS:
			curl->timeout_ms = va_arg(ap, long);
			break;
		case CURLOPT_CONNECTTIMEOUT:
		{
			long sec = va_arg(ap, long);
			curl->connect_timeout_ms = sec > 0 ? sec * 1000L : 0;
			break;
		}
		case CURLOPT_FAILONERROR:
			curl->fail_on_error = va_arg(ap, long) != 0;
			break;
		case CURLOPT_CONNECT_ONLY:
			curl->connect_only = va_arg(ap, long);
			break;
		case CURLOPT_SHARE:
		case CURLOPT_CERTINFO:
		case CURLOPT_SSL_VERIFYPEER:
		case CURLOPT_SSL_VERIFYHOST:
		case CURLOPT_SSL_OPTIONS:
		case CURLOPT_CAINFO_BLOB:
		case CURLOPT_PROXY:
		case CURLOPT_IPRESOLVE:
		case CURLOPT_PROTOCOLS_STR:
		case CURLOPT_REDIR_PROTOCOLS_STR:
		case CURLOPT_OPENSOCKETFUNCTION:
		case CURLOPT_OPENSOCKETDATA:
			(void)va_arg(ap, void *);
			break;
		default:
			res = CURLE_UNKNOWN_OPTION;
			break;
	}

	va_end(ap);
	return res;
}

CURLcode curl_easy_perform(CURL *curl)
{
	if(!curl || !curl->url)
		return CURLE_URL_MALFORMAT;

	curl->response_code = 0;
	curl->ssl_verify_result = 0;

	uint64_t deadline_ms = 0;
	long limit = curl->timeout_ms > 0 ? curl->timeout_ms : curl->connect_timeout_ms;
	if(limit > 0)
		deadline_ms = chiaki_time_now_monotonic_ms() + (uint64_t)limit;

	char current_url[1536];
	snprintf(current_url, sizeof(current_url), "%s", curl->url);

	for(int hop = 0; hop < 4; hop++)
	{
		ParsedUrl u;
		if(!parse_url(current_url, &u))
			return CURLE_URL_MALFORMAT;

		char redirect_url[1536] = {0};
		CURLcode err = perform_http_once(curl, &u, deadline_ms, redirect_url, sizeof(redirect_url));
		if(err != CURLE_OK || !redirect_url[0])
			return err;

		snprintf(current_url, sizeof(current_url), "%s", redirect_url);
	}
	return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
	if(!curl)
		return CURLE_UNKNOWN_OPTION;
	va_list ap;
	va_start(ap, info);
	CURLcode res = CURLE_OK;

	switch(info)
	{
		case CURLINFO_RESPONSE_CODE:
		{
			long *out = va_arg(ap, long *);
			if(out) *out = curl->response_code;
			break;
		}
		case CURLINFO_SSL_VERIFYRESULT:
		{
			long *out = va_arg(ap, long *);
			if(out) *out = curl->ssl_verify_result;
			break;
		}
		case CURLINFO_ACTIVESOCKET:
		{
			curl_socket_t *out = va_arg(ap, curl_socket_t *);
			if(out) *out = curl->sock;
			break;
		}
		case CURLINFO_CAINFO:
		{
			const char **out = va_arg(ap, const char **);
			if(out) *out = NULL;
			break;
		}
		default:
			res = CURLE_UNKNOWN_OPTION;
			break;
	}

	va_end(ap);
	return res;
}

const char *curl_easy_strerror(CURLcode code)
{
	switch(code)
	{
		case CURLE_OK:                     return "No error";
		case CURLE_UNSUPPORTED_PROTOCOL:   return "Unsupported protocol";
		case CURLE_URL_MALFORMAT:          return "URL malformed";
		case CURLE_COULDNT_RESOLVE_HOST:   return "Could not resolve host";
		case CURLE_COULDNT_CONNECT:        return "Could not connect";
		case CURLE_HTTP_RETURNED_ERROR:    return "HTTP response code said error";
		case CURLE_WRITE_ERROR:            return "Write error";
		case CURLE_OUT_OF_MEMORY:          return "Out of memory";
		case CURLE_OPERATION_TIMEDOUT:     return "Operation timed out";
		case CURLE_SSL_CONNECT_ERROR:      return "SSL connect error";
		case CURLE_ABORTED_BY_CALLBACK:    return "Operation aborted by callback";
		case CURLE_GOT_NOTHING:            return "Server returned nothing";
		case CURLE_SEND_ERROR:             return "Send error";
		case CURLE_RECV_ERROR:             return "Receive error";
		case CURLE_AGAIN:                  return "Socket not ready";
		default:                           return "Unknown error";
	}
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *str)
{
	struct curl_slist *node = calloc(1, sizeof(struct curl_slist));
	if(!node)
		return NULL;
	node->data = ps3_strdup(str);
	if(str && !node->data)
	{
		free(node);
		return NULL;
	}
	if(!list)
		return node;
	struct curl_slist *tail = list;
	while(tail->next)
		tail = tail->next;
	tail->next = node;
	return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
	while(list)
	{
		struct curl_slist *next = list->next;
		free(list->data);
		free(list);
		list = next;
	}
}

CURLSH *curl_share_init(void)
{
	return calloc(1, sizeof(CURLSH));
}

void curl_share_cleanup(CURLSH *share)
{
	free(share);
}

const curl_version_info_data *curl_version_info(CURLversion ver)
{
	(void)ver;
	static const curl_version_info_data info = {
		.age = CURLVERSION_NOW,
		.version = "8.0.0-ps3",
		.version_num = 0x080000,
		.host = "powerpc64-ps3-elf",
		.features = 0,
		.ssl_version = "mbedTLS"
	};
	return &info;
}

char *curl_version(void)
{
	return "libcurl/8.0.0-ps3 mbedTLS";
}

// ---------------------------------------------------------------------------
// RFC 6455 WebSocket send / recv

CURLcode curl_ws_send(CURL *curl, const void *buffer, size_t buflen, size_t *sent, curl_off_t fragsize, unsigned int sendflags)
{
	(void)fragsize;
	if(!curl || !curl->connected)
		return CURLE_SEND_ERROR;

	uint8_t opcode = 0x1; // text
	if(sendflags & CURLWS_BINARY)      opcode = 0x2;
	else if(sendflags & CURLWS_CLOSE)  opcode = 0x8;
	else if(sendflags & CURLWS_PING)   opcode = 0x9;
	else if(sendflags & CURLWS_PONG)   opcode = 0xA;
	else if(sendflags & CURLWS_TEXT)   opcode = 0x1;

	uint8_t hdr[14];
	size_t hdr_len = 0;
	hdr[hdr_len++] = (uint8_t)(0x80 | opcode); // FIN=1

	if(buflen < 126)
	{
		hdr[hdr_len++] = (uint8_t)(0x80 | buflen);
	}
	else if(buflen <= 0xFFFF)
	{
		hdr[hdr_len++] = 0x80 | 126;
		hdr[hdr_len++] = (uint8_t)((buflen >> 8) & 0xFF);
		hdr[hdr_len++] = (uint8_t)(buflen & 0xFF);
	}
	else
	{
		hdr[hdr_len++] = 0x80 | 127;
		uint64_t bl = (uint64_t)buflen;
		for(int i = 7; i >= 0; i--)
			hdr[hdr_len++] = (uint8_t)((bl >> (i * 8)) & 0xFF);
	}

	uint8_t mask[4];
	chiaki_random_bytes_crypt(mask, sizeof(mask));
	memcpy(hdr + hdr_len, mask, 4);
	hdr_len += 4;

	uint8_t *frame = malloc(hdr_len + buflen);
	if(!frame)
		return CURLE_OUT_OF_MEMORY;
	memcpy(frame, hdr, hdr_len);
	const uint8_t *src = (const uint8_t *)buffer;
	for(size_t i = 0; i < buflen; i++)
		frame[hdr_len + i] = src[i] ^ mask[i & 3];

	uint64_t deadline = chiaki_time_now_monotonic_ms() + 10000;
	CURLcode err = transport_write_all(curl, frame, hdr_len + buflen, deadline);
	free(frame);
	if(err == CURLE_OK && sent)
		*sent = buflen;
	return err;
}

CURLcode curl_ws_recv(CURL *curl, void *buffer, size_t buflen, size_t *recv_out, const struct curl_ws_frame **meta)
{
	if(recv_out)
		*recv_out = 0;
	if(meta)
		*meta = NULL;
	if(!curl || !curl->connected)
		return CURLE_RECV_ERROR;

	// Drain available non-blocking bytes from TLS into ws_rx_buf
	uint8_t tmp[4096];
	for(int iter = 0; iter < 16; iter++)
	{
		int r = transport_read_some(curl, tmp, sizeof(tmp), true, 0);
		if(r > 0)
		{
			if(!buf_append(&curl->ws_rx_buf, &curl->ws_rx_len, &curl->ws_rx_cap, tmp, (size_t)r))
				return CURLE_OUT_OF_MEMORY;
			continue;
		}
		if(r == -2) // WANT_READ
			break;
		if(r == 0)
		{
			// Peer closed; if we have no buffered frame, report CLOSE
			if(curl->ws_rx_len == 0)
				return CURLE_RECV_ERROR;
			break;
		}
		return CURLE_RECV_ERROR;
	}

	while(true)
	{
		if(curl->ws_rx_len < 2)
			return CURLE_AGAIN;

		uint8_t b0 = curl->ws_rx_buf[0];
		uint8_t b1 = curl->ws_rx_buf[1];
		bool fin = (b0 & 0x80) != 0;
		uint8_t opcode = b0 & 0x0F;
		bool masked = (b1 & 0x80) != 0;
		uint64_t plen = b1 & 0x7F;

		size_t hdr_len = 2;
		if(plen == 126)
		{
			if(curl->ws_rx_len < 4)
				return CURLE_AGAIN;
			plen = ((uint64_t)curl->ws_rx_buf[2] << 8) | curl->ws_rx_buf[3];
			hdr_len = 4;
		}
		else if(plen == 127)
		{
			if(curl->ws_rx_len < 10)
				return CURLE_AGAIN;
			plen = 0;
			for(int i = 0; i < 8; i++)
				plen = (plen << 8) | curl->ws_rx_buf[2 + i];
			hdr_len = 10;
		}

		uint8_t mask[4] = {0};
		if(masked)
		{
			if(curl->ws_rx_len < hdr_len + 4)
				return CURLE_AGAIN;
			memcpy(mask, curl->ws_rx_buf + hdr_len, 4);
			hdr_len += 4;
		}

		if(curl->ws_rx_len < hdr_len + plen)
			return CURLE_AGAIN;

		uint8_t *payload = curl->ws_rx_buf + hdr_len;
		if(masked)
		{
			for(size_t i = 0; i < (size_t)plen; i++)
				payload[i] ^= mask[i & 3];
		}

		// Control frames (PING / PONG / CLOSE) are always delivered immediately
		if(opcode >= 0x8)
		{
			size_t copy_len = (size_t)plen > buflen ? buflen : (size_t)plen;
			if(copy_len > 0 && buffer)
				memcpy(buffer, payload, copy_len);
			buf_consume(curl->ws_rx_buf, &curl->ws_rx_len, hdr_len + (size_t)plen);

			memset(&curl->ws_meta, 0, sizeof(curl->ws_meta));
			if(opcode == 0x8)      curl->ws_meta.flags = CURLWS_CLOSE;
			else if(opcode == 0x9) curl->ws_meta.flags = CURLWS_PING;
			else if(opcode == 0xA) curl->ws_meta.flags = CURLWS_PONG;
			curl->ws_meta.len = copy_len;
			if(recv_out) *recv_out = copy_len;
			if(meta)     *meta = &curl->ws_meta;
			return CURLE_OK;
		}

		// Data frame (TEXT / BINARY / CONT)
		unsigned int frame_flags = 0;
		if(opcode == 0x1)      frame_flags = CURLWS_TEXT;
		else if(opcode == 0x2) frame_flags = CURLWS_BINARY;

		if(!fin)
		{
			if(opcode != 0x0)
			{
				curl->ws_frag_len = 0;
				curl->ws_frag_flags = frame_flags;
			}
			if(!buf_append(&curl->ws_frag_buf, &curl->ws_frag_len, &curl->ws_frag_cap, payload, (size_t)plen))
				return CURLE_OUT_OF_MEMORY;
			buf_consume(curl->ws_rx_buf, &curl->ws_rx_len, hdr_len + (size_t)plen);
			continue;
		}

		if(opcode == 0x0 && curl->ws_frag_len > 0)
		{
			if(!buf_append(&curl->ws_frag_buf, &curl->ws_frag_len, &curl->ws_frag_cap, payload, (size_t)plen))
				return CURLE_OUT_OF_MEMORY;
			buf_consume(curl->ws_rx_buf, &curl->ws_rx_len, hdr_len + (size_t)plen);

			size_t copy_len = curl->ws_frag_len > buflen ? buflen : curl->ws_frag_len;
			if(copy_len > 0 && buffer)
				memcpy(buffer, curl->ws_frag_buf, copy_len);

			memset(&curl->ws_meta, 0, sizeof(curl->ws_meta));
			curl->ws_meta.flags = (int)(curl->ws_frag_flags ? curl->ws_frag_flags : CURLWS_TEXT);
			curl->ws_meta.len = copy_len;
			curl->ws_frag_len = 0;
			curl->ws_frag_flags = 0;
			if(recv_out) *recv_out = copy_len;
			if(meta)     *meta = &curl->ws_meta;
			return CURLE_OK;
		}

		size_t copy_len = (size_t)plen > buflen ? buflen : (size_t)plen;
		if(copy_len > 0 && buffer)
			memcpy(buffer, payload, copy_len);
		buf_consume(curl->ws_rx_buf, &curl->ws_rx_len, hdr_len + (size_t)plen);

		memset(&curl->ws_meta, 0, sizeof(curl->ws_meta));
		curl->ws_meta.flags = (int)(frame_flags ? frame_flags : CURLWS_TEXT);
		curl->ws_meta.len = copy_len;
		if(recv_out) *recv_out = copy_len;
		if(meta)     *meta = &curl->ws_meta;
		return CURLE_OK;
	}
}
