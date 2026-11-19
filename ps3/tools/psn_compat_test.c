// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Host-side verification test for PS3 PSN compatibility layers:
// - ps3_json.c (RFC 8259 parser/serializer + RFC 6901 JSON pointer)
// - ps3_curl.c (HTTP/1.1 + chunked + RFC 6455 WebSocket framing over local loopback)
// - config.c (PSN token, DUID, use_psn, port_guessing round-trip)

#include "config.h"

#include <chiaki/base64.h>
#include <curl/curl.h>
#include <json-c/json_object.h>
#include <json-c/json_tokener.h>
#include <json-c/json_pointer.h>

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_json(void)
{
	const char *sample =
		"{"
		"  \"remotePlaySessions\": ["
		"    {"
		"      \"sessionId\": \"12345678-1234-4123-8123-123456789abc\","
		"      \"members\": ["
		"        {\"accountId\": \"9007199254740993\", \"onlineId\": \"TestUser\"}"
		"      ]"
		"    }"
		"  ],"
		"  \"escaped\": \"line1\\nline2\\\"quote\\\"\\u0041\","
		"  \"num\": -42,"
		"  \"flag\": true,"
		"  \"empty\": {}"
		"}";

	json_tokener *tok = json_tokener_new();
	assert(tok != NULL);
	json_object *root = json_tokener_parse_ex(tok, sample, (int)strlen(sample));
	assert(root != NULL);

	json_object *sid = NULL;
	assert(json_pointer_get(root, "/remotePlaySessions/0/sessionId", &sid) == 0);
	assert(sid && json_object_is_type(sid, json_type_string));
	assert(strcmp(json_object_get_string(sid), "12345678-1234-4123-8123-123456789abc") == 0);

	json_object *aid = NULL;
	assert(json_pointer_get(root, "/remotePlaySessions/0/members/0/accountId", &aid) == 0);
	assert(aid && json_object_is_type(aid, json_type_string));
	assert(json_object_get_int64(aid) == 9007199254740993LL);

	json_object *esc = NULL;
	assert(json_object_object_get_ex(root, "escaped", &esc));
	assert(strcmp(json_object_get_string(esc), "line1\nline2\"quote\"A") == 0);

	json_object *num = NULL;
	assert(json_object_object_get_ex(root, "num", &num));
	assert(json_object_is_type(num, json_type_int));
	assert(json_object_get_int(num) == -42);

	json_object *empty = NULL;
	assert(json_object_object_get_ex(root, "empty", &empty));
	assert(json_object_object_length(empty) == 0);

	const char *ser = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
	assert(ser && strstr(ser, "12345678-1234-4123-8123-123456789abc") != NULL);

	json_object_put(root);
	json_tokener_free(tok);
	printf("[PASS] ps3_json\n");
}

typedef struct
{
	char *data;
	size_t size;
} Buf;

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *user)
{
	Buf *b = user;
	size_t n = sz * nm;
	b->data = realloc(b->data, b->size + n + 1);
	memcpy(b->data + b->size, ptr, n);
	b->size += n;
	b->data[b->size] = '\0';
	return n;
}

static void *mock_http_server(void *arg)
{
	int lfd = *(int *)arg;
	int cfd = accept(lfd, NULL, NULL);
	if(cfd < 0)
		return NULL;
	char req[1024];
	ssize_t r = recv(cfd, req, sizeof(req) - 1, 0);
	if(r > 0)
	{
		req[r] = '\0';
		const char *resp =
			"HTTP/1.1 200 OK\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Content-Type: application/json\r\n"
			"\r\n"
			"a\r\n"
			"{\"ok\":true\r\n"
			"1\r\n"
			"}\r\n"
			"0\r\n\r\n";
		send(cfd, resp, strlen(resp), 0);
	}
	close(cfd);
	return NULL;
}

static void *mock_ws_server(void *arg)
{
	int lfd = *(int *)arg;
	int cfd = accept(lfd, NULL, NULL);
	if(cfd < 0)
		return NULL;
	char req[2048];
	ssize_t r = recv(cfd, req, sizeof(req) - 1, 0);
	if(r > 0)
	{
		const char *resp =
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: dummy\r\n"
			"\r\n";
		send(cfd, resp, strlen(resp), 0);

		// Receive masked client PING frame (6 bytes: 0x89 0x80 + 4 mask bytes)
		uint8_t ping[64];
		ssize_t pr = recv(cfd, ping, sizeof(ping), 0);
		assert(pr >= 6);
		assert((ping[0] & 0x0F) == 0x9); // PING
		assert((ping[1] & 0x80) != 0);   // masked

		// Reply with PONG + fragmented TEXT frame
		uint8_t pong[2] = { 0x8A, 0x00 };
		send(cfd, pong, sizeof(pong), 0);

		const char *p1 = "{\"msg\":";
		const char *p2 = "\"hello\"}";
		uint8_t f1[32];
		f1[0] = 0x01; // FIN=0, TEXT
		f1[1] = (uint8_t)strlen(p1);
		memcpy(f1 + 2, p1, strlen(p1));
		send(cfd, f1, 2 + strlen(p1), 0);

		uint8_t f2[32];
		f2[0] = 0x80; // FIN=1, CONT
		f2[1] = (uint8_t)strlen(p2);
		memcpy(f2 + 2, p2, strlen(p2));
		send(cfd, f2, 2 + strlen(p2), 0);
	}
	usleep(100000);
	close(cfd);
	return NULL;
}

static int listen_loopback(uint16_t *port_out)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	socklen_t slen = sizeof(sa);
	assert(getsockname(fd, (struct sockaddr *)&sa, &slen) == 0);
	assert(listen(fd, 2) == 0);
	*port_out = ntohs(sa.sin_port);
	return fd;
}

static void test_http_and_ws(void)
{
	// 1. Chunked HTTP test
	uint16_t port = 0;
	int lfd = listen_loopback(&port);
	pthread_t th;
	assert(pthread_create(&th, NULL, mock_http_server, &lfd) == 0);

	char url[128];
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);
	CURL *curl = curl_easy_init();
	Buf b = { NULL, 0 };
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	assert(curl_easy_perform(curl) == CURLE_OK);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	assert(code == 200);
	assert(b.data && strcmp(b.data, "{\"ok\":true}") == 0);
	free(b.data);
	curl_easy_cleanup(curl);
	pthread_join(th, NULL);
	close(lfd);

	// 2. WebSocket test (upgrade + PING + PONG + fragmented TEXT)
	lfd = listen_loopback(&port);
	assert(pthread_create(&th, NULL, mock_ws_server, &lfd) == 0);
	snprintf(url, sizeof(url), "ws://127.0.0.1:%u/np/pushNotification", (unsigned)port);
	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
	assert(curl_easy_perform(curl) == CURLE_OK);

	size_t sent = 0;
	assert(curl_ws_send(curl, "", 0, &sent, 0, CURLWS_PING) == CURLE_OK);

	char wsbuf[256];
	size_t rlen = 0;
	const struct curl_ws_frame *meta = NULL;
	CURLcode r = CURLE_AGAIN;
	for(int i = 0; i < 50 && r == CURLE_AGAIN; i++)
	{
		r = curl_ws_recv(curl, wsbuf, sizeof(wsbuf) - 1, &rlen, &meta);
		if(r == CURLE_AGAIN) usleep(10000);
	}
	assert(r == CURLE_OK && meta && (meta->flags & CURLWS_PONG));

	r = CURLE_AGAIN;
	for(int i = 0; i < 50 && r == CURLE_AGAIN; i++)
	{
		r = curl_ws_recv(curl, wsbuf, sizeof(wsbuf) - 1, &rlen, &meta);
		if(r == CURLE_AGAIN) usleep(10000);
	}
	assert(r == CURLE_OK && meta && (meta->flags & CURLWS_TEXT));
	wsbuf[rlen] = '\0';
	assert(strcmp(wsbuf, "{\"msg\":\"hello\"}") == 0);

	curl_easy_cleanup(curl);
	pthread_join(th, NULL);
	close(lfd);
	printf("[PASS] ps3_curl (HTTP chunked + WebSocket)\n");
}

static void test_config_roundtrip(void)
{
	Config cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.path, sizeof(cfg.path), "/tmp/chiaki_ps3_test.cfg");
	cfg.resolution = 720;
	cfg.fps = 60;
	cfg.pacing = 1;
	cfg.low_latency = true;
	cfg.vibration = true;
	cfg.port_guessing = true;
	cfg.have_account_id = true;
	for(int i = 0; i < 8; i++) cfg.psn_account_id[i] = (uint8_t)(i + 1);
	snprintf(cfg.psn_refresh_token, sizeof(cfg.psn_refresh_token), "rt_test_token_12345");
	snprintf(cfg.psn_auth_token, sizeof(cfg.psn_auth_token), "at_test_token_67890");

	ConfigHost *h = config_add_host(&cfg);
	assert(h != NULL);
	snprintf(h->name, sizeof(h->name), "My Remote PS5");
	h->ps5 = true;
	h->use_psn = true;
	h->have_duid = true;
	for(int i = 0; i < 32; i++) h->duid[i] = (uint8_t)(0xA0 + i);

	assert(config_save(&cfg));

	Config loaded;
	assert(config_import(&loaded, "/tmp/chiaki_ps3_test.cfg"));
	assert(loaded.have_account_id);
	assert(memcmp(loaded.psn_account_id, cfg.psn_account_id, 8) == 0);
	assert(strcmp(loaded.psn_refresh_token, "rt_test_token_12345") == 0);
	assert(strcmp(loaded.psn_auth_token, "at_test_token_67890") == 0);
	assert(loaded.port_guessing == true);
	assert(loaded.host_count == 1);
	assert(loaded.hosts[0].ps5 == true);
	assert(loaded.hosts[0].use_psn == true);
	assert(loaded.hosts[0].have_duid == true);
	assert(memcmp(loaded.hosts[0].duid, h->duid, 32) == 0);
	assert(config_host_connectable(&loaded, &loaded.hosts[0]));
	remove("/tmp/chiaki_ps3_test.cfg");
	printf("[PASS] config PSN round-trip\n");
}

int main(void)
{
	test_json();
	test_http_and_ws();
	test_config_roundtrip();
	printf("All PS3 PSN compatibility tests PASSED!\n");
	return 0;
}
