// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Chiaki PS3: PS4 Remote Play client.
//
// Flow: read chiaki.cfg -> register with the PS4 if needed (PIN) -> connect ->
// stream (video via libvdec + RSX, audio via libaudio, DualShock 3 input).
// Status and errors are shown with XMB message dialogs; everything is also
// logged to chiaki.log.

#include "app.h"
#include "audio.h"
#include "config.h"
#include "discover.h"
#include "input.h"
#include "menu_shell.h"
#include "psn.h"
#include "ui.h"
#include "video.h"

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>
#include <chiaki/thread.h>
#include <chiaki/time.h>

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/net.h>
#include <net/netctl.h>
#include <sys/process.h>
#include <lv2/process.h>
#include <sys/systime.h>
#include <rsx/gcm_sys.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/msg.h>
#include <sysutil/osk.h>
#include <sys/memory.h>
#include <sysutil/sysutil.h>

// Main thread runs the display loop: above the chiaki network threads (1000)
// so it does not miss vsyncs; it sleeps while waiting for the flip.
SYS_PROCESS_PARAM(700, 0x100000);

// ---------------------------------------------------------------------------
// logging

static FILE *log_file;
static ChiakiMutex log_mutex;
ChiakiLog app_log_ctx;

static void log_line(const char *prefix, const char *msg)
{
	chiaki_mutex_lock(&log_mutex);
	if(log_file)
	{
		fprintf(log_file, "[%8llu] %s%s\n", (unsigned long long)chiaki_time_now_monotonic_ms(), prefix, msg);
		fflush(log_file);
	}
	printf("%s%s\n", prefix, msg); // visible on a debug TTY / ps3load
	chiaki_mutex_unlock(&log_mutex);
}

void app_log(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	log_line("[app] ", buf);
}

static void chiaki_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	const char *p = "[?] ";
	switch(level)
	{
		case CHIAKI_LOG_DEBUG: p = "[D] "; break;
		case CHIAKI_LOG_VERBOSE: p = "[V] "; break;
		case CHIAKI_LOG_INFO: p = "[I] "; break;
		case CHIAKI_LOG_WARNING: p = "[W] "; break;
		case CHIAKI_LOG_ERROR: p = "[E] "; break;
	}
	log_line(p, msg);
}

// ---------------------------------------------------------------------------
// system / dialogs

volatile bool app_exit;
static volatile bool dialog_open;
static volatile bool dialog_answered;

// On-screen keyboard state (see osk_input)
#define OSK_TEXT_MAX 64
static volatile bool osk_running;
static oskCallbackReturnParam osk_out;
static uint16_t osk_title16[96], osk_initial16[OSK_TEXT_MAX + 1], osk_text16[OSK_TEXT_MAX + 1];

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param; (void)user;
	switch(status)
	{
		case SYSUTIL_EXIT_GAME:
			app_exit = true;
			break;
		case SYSUTIL_OSK_INPUT_CANCELED:
			oskAbort();
			// fall through
		case SYSUTIL_OSK_DONE:
			oskUnloadAsync(&osk_out);
			break;
		case SYSUTIL_OSK_UNLOADED:
			osk_running = false;
			break;
		default:
			break;
	}
}

static void ascii_to_utf16(const char *src, uint16_t *dst, size_t dst_count)
{
	size_t i = 0;
	for(; src[i] && i + 1 < dst_count; i++)
		dst[i] = (uint16_t)(unsigned char)src[i];
	dst[i] = 0;
}

// The PS3's own on-screen keyboard. Returns 1 with the text in out, 0 if it was
// cancelled or left empty, -1 if the keyboard could not be opened.
int osk_input(const char *title, const char *initial, char *out, size_t out_size)
{
	dialog_close();
	ascii_to_utf16(title, osk_title16, sizeof(osk_title16) / sizeof(osk_title16[0]));
	ascii_to_utf16(initial ? initial : "", osk_initial16, OSK_TEXT_MAX + 1);
	memset(osk_text16, 0, sizeof(osk_text16));

	oskInputFieldInfo field;
	memset(&field, 0, sizeof(field));
	field.message = osk_title16;
	field.startText = osk_initial16;
	field.maxLength = OSK_TEXT_MAX;

	oskParam param;
	memset(&param, 0, sizeof(param));
	param.allowedPanels = OSK_PANEL_TYPE_DEFAULT;
	param.firstViewPanel = OSK_PANEL_TYPE_DEFAULT;
	param.prohibitFlags = OSK_PROHIBIT_RETURN;

	memset(&osk_out, 0, sizeof(osk_out));
	osk_out.res = OSK_CANCELED;
	osk_out.len = OSK_TEXT_MAX;
	osk_out.str = osk_text16;

	sys_mem_container_t container;
	if(sysMemContainerCreate(&container, 4 * 1024 * 1024) != 0)
	{
		app_log("On-screen keyboard: no memory container");
		return -1;
	}
	oskSetInitialInputDevice(OSK_DEVICE_PAD);
	oskSetKeyLayoutOption(OSK_FULLKEY_PANEL);
	oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);
	osk_running = true;
	s32 r = oskLoadAsync(container, &param, &field);
	if(r != 0)
	{
		osk_running = false;
		app_log("On-screen keyboard: could not open (0x%x)", (unsigned)r);
		sysMemContainerDestroy(container);
		return -1;
	}
	while(osk_running && !app_exit)
	{
		sysUtilCheckCallback();
		video_display_present();
	}
	sysMemContainerDestroy(container);
	if(osk_running || osk_out.res != OSK_OK)
	{
		osk_running = false;
		return 0;
	}
	size_t n = 0;
	for(; osk_text16[n] && n < OSK_TEXT_MAX && n + 1 < out_size; n++)
		out[n] = osk_text16[n] < 0x80 ? (char)osk_text16[n] : '?';
	out[n] = '\0';
	return n > 0 ? 1 : 0;
}

static void dialog_cb(msgButton button, void *user)
{
	(void)button; (void)user;
	dialog_open = false;
	dialog_answered = true;
}

void dialog_close(void)
{
	if(dialog_open)
	{
		msgDialogAbort();
		dialog_open = false;
	}
}

// Status text without buttons; stays until replaced or closed.
void dialog_status(const char *text)
{
	dialog_close();
	dialog_answered = false;
	if(msgDialogOpen2(MSG_DIALOG_NORMAL | MSG_DIALOG_DISABLE_CANCEL_ON, text, dialog_cb, NULL, NULL) == 0)
		dialog_open = true;
}

// Message with an OK button; runs the display loop until it is dismissed.
void dialog_wait_ok(bool error, const char *fmt, ...)
{
	char text[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	app_log("Dialog: %s", text);

	dialog_close();
	dialog_answered = false;
	msgType type = (error ? MSG_DIALOG_ERROR : MSG_DIALOG_NORMAL) | MSG_DIALOG_BTN_TYPE_OK;
	if(msgDialogOpen2(type, text, dialog_cb, NULL, NULL) != 0)
		return;
	dialog_open = true;
	while(!dialog_answered && !app_exit)
	{
		sysUtilCheckCallback();
		video_display_present();
	}
	dialog_open = false;
}

static volatile msgButton dialog_button;

static void dialog_cb_button(msgButton button, void *user)
{
	(void)user;
	dialog_button = button;
	dialog_open = false;
	dialog_answered = true;
}

bool dialog_yes_no(const char *text)
{
	dialog_close();
	dialog_answered = false;
	dialog_button = MSG_DIALOG_BTN_NO;
	if(msgDialogOpen2(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_YESNO | MSG_DIALOG_DEFAULT_CURSOR_NO, text,
				dialog_cb_button, NULL, NULL) != 0)
		return false;
	dialog_open = true;
	while(!dialog_answered && !app_exit)
	{
		sysUtilCheckCallback();
		video_display_present();
	}
	dialog_open = false;
	return dialog_button == MSG_DIALOG_BTN_YES;
}

static void log_ip(void)
{
	union net_ctl_info info;
	if(netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) == 0)
		app_log("PS3 IP address: %s", info.ip_address);
	else
		app_log("PS3 has no IP address (network not connected?)");
}

// ---------------------------------------------------------------------------
// exit watchdog: a stuck thread must never keep the app from closing
// (otherwise the PS3 cannot return to the XMB and needs a reboot)

static volatile uint64_t watchdog_deadline_ms; // 0 = disarmed
static ChiakiThread watchdog_thread;

// Main loop heartbeat: if the display loop stops advancing, the watchdog logs
// where it is stuck (the loop itself cannot log then).
static volatile bool main_watch;
static volatile uint64_t main_tick_ms;
static volatile const char *main_stage = "-";

static volatile uint64_t input_tick_ms;    // input thread heartbeat (0 = not running)
static volatile const char *input_stage = "-";

static void main_tick(const char *stage)
{
	main_stage = stage;
	main_tick_ms = chiaki_time_now_monotonic_ms();
}

static void *watchdog_func(void *arg)
{
	(void)arg;
	while(true)
	{
		sysUsleep(100 * 1000);
		static uint64_t reported_tick;
		uint64_t tick = main_tick_ms;
		if(main_watch && tick && tick != reported_tick && chiaki_time_now_monotonic_ms() - tick > 2000)
		{
			reported_tick = tick;
			app_log("Display loop stuck for >2 s in \"%s\" (video: %s, flip status %u)",
					(const char *)main_stage, (const char *)video_present_stage, (unsigned)gcmGetFlipStatus());
		}
		static uint64_t reported_input_tick;
		uint64_t itick = input_tick_ms;
		if(main_watch && itick && itick != reported_input_tick && chiaki_time_now_monotonic_ms() - itick > 2000)
		{
			reported_input_tick = itick;
			app_log("Input thread stuck for >2 s in \"%s\"", (const char *)input_stage);
		}
		uint64_t deadline = watchdog_deadline_ms;
		if(deadline && chiaki_time_now_monotonic_ms() > deadline)
		{
			app_log("Shutdown is stuck, forcing exit");
			if(log_file)
				fflush(log_file);
			sysProcessExit(0);
		}
	}
	return NULL;
}

static void watchdog_arm(uint32_t timeout_ms)
{
	watchdog_deadline_ms = chiaki_time_now_monotonic_ms() + timeout_ms;
}

static void watchdog_disarm(void)
{
	watchdog_deadline_ms = 0;
}

// ---------------------------------------------------------------------------
// finding / waking the console

bool keep_display_alive(void)
{
	sysUtilCheckCallback();
	video_display_present();
	return !app_exit;
}

// Fills h->host from the network if needed and wakes the console from rest mode.
// Returns false if there is no usable host on LAN.
static bool find_console(Config *cfg, ConfigHost *h, bool show_not_found_error)
{
	(void)cfg;
	struct in_addr tmp;
	bool host_is_ip = h->host[0] && inet_aton(h->host, &tmp);

	dialog_status(h->ps5 ? "Looking for your PS5 on the network..." : "Looking for your PS4 on the network...");
	FoundHost found;
	bool ok = discover_console(&app_log_ctx, h->ps5, host_is_ip ? h->host : NULL, h->host_id, 6000, keep_display_alive, &found);
	if(app_exit)
		return false;

	if(!ok)
	{
		if(h->host[0] && show_not_found_error)
		{
			// It may simply not answer discovery (e.g. broadcasts filtered); try the configured address.
			app_log("Console did not answer discovery, trying %s anyway", h->host);
			return true;
		}
		if(show_not_found_error)
			dialog_wait_ok(true, "Could not find the console on the network.\n\n"
					"Make sure it is on (or in rest mode with Remote Play allowed from rest mode) and on the same network, "
					"or set its IP address with Options > Set IP address.");
		return false;
	}

	if(!h->name[0] && found.name[0])
	{
		snprintf(h->name, sizeof(h->name), "%.31s", found.name);
		if(!config_save(cfg))
			app_log("Could not save the console name %s", h->name);
	}

	struct in_addr found_ip;
	bool found_addr_ok = inet_aton(found.addr, &found_ip) && found_ip.s_addr != 0;
	if(found_addr_ok)
		snprintf(h->host, sizeof(h->host), "%s", found.addr);
	else if(!h->host[0])
	{
		dialog_wait_ok(true, "Found the console, but could not read its IP address (%s).\n\nSet host= to its IP address in\n%s",
				found.addr, cfg->path);
		return false;
	}
	else
		app_log("Discovery returned unusable address %s, keeping %s", found.addr, h->host);
	if(found.state != CHIAKI_DISCOVERY_HOST_STATE_STANDBY)
		return true;

	if(!config_host_registered(h))
	{
		dialog_wait_ok(true, "The console is in rest mode. Turn it on to register this PS3.");
		return false;
	}

	// Wake it up and wait until it reports ready.
	dialog_status(h->ps5 ? "Waking up your PS5..." : "Waking up your PS4...");
	uint64_t start = chiaki_time_now_monotonic_ms();
	uint64_t last_wake = 0;
	while(!app_exit && chiaki_time_now_monotonic_ms() - start < 60 * 1000)
	{
		uint64_t now = chiaki_time_now_monotonic_ms();
		if(!last_wake || now - last_wake >= 10 * 1000)
		{
			last_wake = now;
			discover_wakeup(&app_log_ctx, h->host, h->regist_key, h->ps5);
		}
		FoundHost fh;
		if(discover_console(&app_log_ctx, h->ps5, h->host, found.id, 2000, keep_display_alive, &fh)
				&& fh.state == CHIAKI_DISCOVERY_HOST_STATE_READY)
		{
			struct in_addr ip;
			if(inet_aton(fh.addr, &ip) && ip.s_addr != 0)
				snprintf(h->host, sizeof(h->host), "%s", fh.addr);
			app_log("Console is awake");
			return true;
		}
	}
	if(!app_exit)
		dialog_wait_ok(true, h->ps5 ? "Your PS5 did not wake up.\n\nCheck PS5 Settings > System > Power Saving > Features Available in Rest Mode: "
				"\"Stay Connected to the Internet\" and \"Enable Turning On PS5 from Network\"."
				: "Your PS4 did not wake up.\n\nCheck PS4 Settings > Power Save Settings > "
				"Set Features Available in Rest Mode: \"Stay Connected to the Internet\" and "
				"\"Enable Turning On PS4 from Network\".");
	return false;
}

// ---------------------------------------------------------------------------
// session callbacks

typedef struct
{
	FILE *dump;
	volatile bool connected;
	volatile bool quit;
	volatile bool pin_requested;
	volatile bool data_holepunch_active;
	ChiakiQuitReason quit_reason;
	char quit_reason_str[256];
	uint64_t frames_lost;
	volatile uint64_t video_bytes;
} StreamCtx;

static bool video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	(void)frame_recovered;
	StreamCtx *ctx = user;
	if(frames_lost > 0)
		ctx->frames_lost += (uint64_t)frames_lost;
	ctx->video_bytes += buf_size;
	if(ctx->dump)
		fwrite(buf, 1, buf_size, ctx->dump);
	return video_decoder_decode(buf, buf_size);
}

static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	(void)user;
	audio_set_format(channels, rate);
}

static void audio_frame_cb(int16_t *buf, size_t samples_count, void *user)
{
	(void)user;
	audio_push(buf, samples_count);
}

static void event_cb(ChiakiEvent *event, void *user)
{
	StreamCtx *ctx = user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			app_log("Session connected");
			ctx->connected = true;
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			app_log("Console requests a login PIN");
			ctx->pin_requested = true;
			break;
		case CHIAKI_EVENT_RUMBLE:
			input_set_rumble(event->rumble.left, event->rumble.right);
			break;
		case CHIAKI_EVENT_HOLEPUNCH:
			ctx->data_holepunch_active = !event->data_holepunch.finished;
			app_log("PSN data holepunch %s", event->data_holepunch.finished ? "finished" : "started");
			break;
		case CHIAKI_EVENT_QUIT:
			ctx->quit_reason = event->quit.reason;
			snprintf(ctx->quit_reason_str, sizeof(ctx->quit_reason_str), "%s",
					event->quit.reason_str ? event->quit.reason_str : "");
			app_log("Session quit: %s %s", chiaki_quit_reason_string(event->quit.reason), ctx->quit_reason_str);
			ctx->quit = true;
			break;
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// input thread: polls the pad and hands the state to chiaki, so that a chiaki
// lock can never stall the display loop

typedef struct
{
	ChiakiSession *session;
	ChiakiThread thread;
	volatile bool run;
	volatile bool connected;
	volatile bool quit_requested;
	volatile bool stats_toggle;
} InputCtx;

static void *input_thread_func(void *arg)
{
	InputCtx *ic = arg;
	ChiakiControllerState pad, last_pad;
	chiaki_controller_state_set_idle(&last_pad);
	while(ic->run)
	{
		input_tick_ms = chiaki_time_now_monotonic_ms();
		input_stage = "pad";
		InputAction action = input_poll(&pad);
		if(action == INPUT_ACTION_QUIT)
			ic->quit_requested = true;
		else if(action == INPUT_ACTION_TOGGLE_STATS)
			ic->stats_toggle = true;
		if(ic->connected && !chiaki_controller_state_equals(&pad, &last_pad))
		{
			input_stage = "set_controller_state";
			chiaki_session_set_controller_state(ic->session, &pad);
			last_pad = pad;
		}
		input_stage = "sleep";
		sysUsleep(2 * 1000); // ~500 Hz: a press waits ~1 ms on average before it is seen
	}
	input_tick_ms = 0;
	return NULL;
}

// ---------------------------------------------------------------------------
// streaming

#define SESSION_ATTEMPTS 4

void chiaki_ps3_net_error(const char *what, int err, int fd)
{
	app_log("Network: %s failed on socket 0x%x: errno %d (%s)", what, (unsigned)fd, err, strerror(err));
}

// Failures right at the start that are worth another try (the console may still
// be closing a previous session).
static bool quit_reason_retryable(ChiakiQuitReason r)
{
	return r == CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED
		|| r == CHIAKI_QUIT_REASON_CTRL_UNKNOWN || r == CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED
		|| r == CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED;
}

// Stats overlay (SELECT + TRIANGLE): two lines over the top-left corner of the video.
static bool overlay_enabled; // kept between streams
static uint32_t overlay_px[VIDEO_OVERLAY_W * VIDEO_OVERLAY_H];

typedef struct
{
	uint64_t at_ms;
	uint64_t in, shown, skipped, lost, bytes;
} OverlaySample;

static void overlay_update(OverlaySample *prev, const StreamCtx *ctx, uint64_t now_ms)
{
	VideoStats vs;
	AudioStats as;
	video_peek_stats(&vs);
	audio_get_stats(&as);
	uint64_t dt = now_ms - prev->at_ms;
	if(!dt)
		dt = 1;
	uint64_t bytes = ctx->video_bytes;
	unsigned in10 = (unsigned)((vs.frames_in - prev->in) * 10000 / dt);
	unsigned shown10 = (unsigned)((vs.frames_presented - prev->shown) * 10000 / dt);
	unsigned kbit = (unsigned)((bytes - prev->bytes) * 8 / dt);
	char l1[128], l2[128];
	snprintf(l1, sizeof(l1), "%ux%u   in %u.%u fps   shown %u.%u fps   skipped %llu   lost %llu",
			(unsigned)vs.width, (unsigned)vs.height, in10 / 10, in10 % 10, shown10 / 10, shown10 % 10,
			(unsigned long long)(vs.frames_skipped - prev->skipped), (unsigned long long)(ctx->frames_lost - prev->lost));
	snprintf(l2, sizeof(l2), "decoder %u ms   audio buffer %u ms   %u.%u Mbit/s",
			(unsigned)vs.decode_ms_avg, (unsigned)as.buffered_ms, kbit / 1000, kbit % 1000 / 100);
	prev->at_ms = now_ms;
	prev->in = vs.frames_in;
	prev->shown = vs.frames_presented;
	prev->skipped = vs.frames_skipped;
	prev->lost = ctx->frames_lost;
	prev->bytes = bytes;

	Canvas c = { overlay_px, VIDEO_OVERLAY_W, VIDEO_OVERLAY_H };
	ui_fill(&c, 0, 0, VIDEO_OVERLAY_W, VIDEO_OVERLAY_H, 0x0b1220);
	ui_fill(&c, 0, VIDEO_OVERLAY_H - 2, VIDEO_OVERLAY_W, 2, 0x6fb6ff);
	ui_text_fit(&c, &font_small, 12, 5, l1, 0xf2f4f8, VIDEO_OVERLAY_W - 24);
	ui_text_fit(&c, &font_small, 12, 32, l2, 0x9aa6b8, VIDEO_OVERLAY_W - 24);
	video_display_set_overlay(overlay_px);
}

// Plain-language explanation of why a session ended.
static const char *explain_quit(ChiakiQuitReason r, bool ps5, bool was_connected)
{
	switch(r)
	{
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
			return ps5 ? "The PS5 is not accepting connections.\n\nIt may still be starting up (wait a moment), or "
					"Remote Play is switched off: Settings > System > Remote Play > Enable Remote Play."
				: "The PS4 is not accepting connections.\n\nIt may still be starting up (wait a moment), or "
					"Remote Play is switched off: Settings > Remote Play Connection Settings > Enable Remote Play.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
			return "Remote Play is already in use on this console.\n\nDisconnect the other device, or wait a moment "
				"if you just ended a stream, and try again.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
			return "Remote Play on the console has stopped working.\n\nRestart the console and try again.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
			return "The console's system software uses a Remote Play version this app does not know.\n\n"
				"Check the console type (Options > Console type) and look for a newer version of this app.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN:
			return "The console did not accept the connection.\n\nMost often the pairing is no longer valid "
				"(for example after signing out on the console): use Options > Register again with PIN. "
				"Also check that you are logged in on the console.";
		case CHIAKI_QUIT_REASON_CTRL_UNKNOWN:
		case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED:
		case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
			return was_connected ? "The connection to the console was lost.\n\nCheck the network cables and try again."
				: "The console is not ready for a new connection yet.\n\nThis happens right after another stream "
				  "ended. Wait a few seconds and try again.";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN:
			return was_connected ? "The stream stopped unexpectedly.\n\nCheck the network connection of both consoles "
					"(wired is best) and try again."
				: "The stream could not be started.\n\nCheck the network connection of both consoles and try again. "
				  "If it keeps happening, try a lower resolution or bitrate in the settings.";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED:
			return "The console ended the stream.\n\nThis happens when someone starts using the console itself, "
				"another device connects, or the console goes to rest mode.";
		case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
			return "PSN auto-authentication with the console failed.\n\nCheck that your PSN account ID matches the "
				"account signed in on the PS5/PS4 and that Remote Play is enabled.";
		default:
			return "The connection ended.";
	}
}

// Returns true if the session failed before it connected and may be retried
// (then no error dialog was shown).
// Takes ownership of hp (if non-NULL): chiaki_session_fini will free it.
static bool run_session(Config *cfg, ConfigHost *host, ChiakiHolepunchSession hp, bool last_attempt)
{
	bool retry = false;
	chiaki_log_set_level(&app_log_ctx, cfg->verbose_log ? CHIAKI_LOG_ALL : (CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG)));
	app_log("Connecting to %s \"%s\" (%s), %dp@%d bitrate=%d pacing=%d low_latency=%d vibration=%d",
			host->ps5 ? "PS5" : "PS4", host->name, hp ? "PSN Internet" : host->host,
			cfg->resolution, cfg->fps, cfg->bitrate, cfg->pacing, cfg->low_latency ? 1 : 0, cfg->vibration ? 1 : 0);
	video_set_pacing(cfg->pacing);
	video_set_low_latency_decode(cfg->low_latency);
	video_set_late_latch(cfg->low_latency);
	input_set_rumble(0, 0);
	input_set_rumble_enabled(cfg->vibration);
	audio_reset();
	video_display_hide_ui();
	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.ps5 = host->ps5;
	ci.enable_dualsense = false; // a DS3 has no adaptive triggers / haptics
	// Keep the console's own on-screen keyboard in the stream (typed on with the
	// pad) instead of handing text entry to the client.
	ci.enable_keyboard = false;
	ci.holepunch_session = hp;
	if(hp)
		memcpy(ci.psn_account_id, cfg->psn_account_id, sizeof(ci.psn_account_id));
	else
	{
		ci.host = host->host;
		memcpy(ci.regist_key, host->regist_key, sizeof(ci.regist_key));
		memcpy(ci.morning, host->morning, sizeof(ci.morning));
	}
	ChiakiVideoResolutionPreset res = cfg->resolution >= 720 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p
		: cfg->resolution >= 540 ? CHIAKI_VIDEO_RESOLUTION_PRESET_540p : CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
	chiaki_connect_video_profile_preset(&ci.video_profile, res,
			cfg->fps >= 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
	ci.video_profile.codec = CHIAKI_CODEC_H264;
	if(cfg->bitrate > 0)
		ci.video_profile.bitrate = (unsigned int)cfg->bitrate;
	ci.video_profile_auto_downgrade = true;
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true;

	StreamCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	if(cfg->dump_stream)
		ctx.dump = fopen(STREAM_PATH, "wb");

	if(!video_decoder_init())
	{
		if(hp)
			chiaki_holepunch_session_fini(hp);
		dialog_wait_ok(true, "Could not start the PS3 video decoder. See %s", LOG_PATH);
		goto out_dump;
	}

	ChiakiOpusDecoder opus;
	chiaki_opus_decoder_init(&opus, &app_log_ctx);
	chiaki_opus_decoder_set_cb(&opus, audio_settings_cb, audio_frame_cb, NULL);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&opus, &audio_sink);

	ChiakiSession session;
	ChiakiErrorCode err = chiaki_session_init(&session, &ci, &app_log_ctx);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		dialog_wait_ok(true, "Could not start the connection (%s).\n\nCheck the console's IP address in Options > Set IP address.", chiaki_error_string(err));
		goto out_decoders;
	}
	chiaki_session_set_audio_sink(&session, &audio_sink);
	chiaki_session_set_video_sample_cb(&session, video_cb, &ctx);
	chiaki_session_set_event_cb(&session, event_cb, &ctx);

	dialog_status(host->ps5 ? "Connecting to the PS5..." : "Connecting to the PS4...");
	err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		dialog_wait_ok(true, "Could not start the session: %s", chiaki_error_string(err));
		chiaki_session_fini(&session);
		goto out_decoders;
	}

	input_session_reset();
	InputCtx input;
	memset(&input, 0, sizeof(input));
	input.session = &session;
	input.run = true;
	bool input_thread_ok = chiaki_thread_create(&input.thread, input_thread_func, &input) == CHIAKI_ERR_SUCCESS;
	if(!input_thread_ok)
		app_log("Could not start the input thread");
	main_tick("loop");
	main_watch = true;

	bool was_connected = false;
	bool user_quit = false;
	bool stalled = false;
	uint64_t last_frames_in = 0;
	uint64_t last_frame_ms = 0;
	uint64_t last_stats = chiaki_time_now_monotonic_ms();
	OverlaySample overlay_last;
	memset(&overlay_last, 0, sizeof(overlay_last));
	video_display_set_overlay(NULL);

	while(!app_exit && !ctx.quit)
	{
		main_tick("sysutil");
		sysUtilCheckCallback();
		main_tick("loop");

		if(ctx.connected && !was_connected)
		{
			was_connected = true;
			dialog_close();
		}
		if(ctx.pin_requested)
		{
			ctx.pin_requested = false;
			app_log("Login PIN is not supported yet, stopping");
			chiaki_session_stop(&session);
		}

		input.connected = was_connected;
		if(input.quit_requested)
		{
			app_log("Quit requested with SELECT + CIRCLE");
			user_quit = true;
			break;
		}

		main_tick("present");
		video_display_present(); // waits for vsync
		main_tick("loop");

		// Stall detection: video was flowing and stopped without a quit event.
		{
			VideoStats vs_now;
			video_peek_stats(&vs_now);
			uint64_t t = chiaki_time_now_monotonic_ms();
			if(vs_now.frames_in != last_frames_in)
			{
				last_frames_in = vs_now.frames_in;
				last_frame_ms = t;
			}
			else if(last_frames_in > 0 && t - last_frame_ms > 10 * 1000)
			{
				app_log("No video for 10 s, treating the connection as lost");
				stalled = true;
				break;
			}
		}

		uint64_t now = chiaki_time_now_monotonic_ms();
		if(input.stats_toggle)
		{
			input.stats_toggle = false;
			overlay_enabled = !overlay_enabled;
			app_log("Stats overlay %s", overlay_enabled ? "on" : "off");
			if(!overlay_enabled)
				video_display_set_overlay(NULL);
			overlay_last.at_ms = 0; // draw right away
		}
		if(overlay_enabled && was_connected && now - overlay_last.at_ms >= 1000)
		{
			if(!overlay_last.at_ms)
			{
				// first sample: start the one-second window now
				overlay_last.at_ms = now - 1;
				VideoStats vs0;
				video_peek_stats(&vs0);
				overlay_last.in = vs0.frames_in;
				overlay_last.shown = vs0.frames_presented;
				overlay_last.skipped = vs0.frames_skipped;
				overlay_last.lost = ctx.frames_lost;
				overlay_last.bytes = ctx.video_bytes;
			}
			overlay_update(&overlay_last, &ctx, now);
		}
		if(now - last_stats >= 5000)
		{
			main_tick("stats");
			last_stats = now;
			VideoStats vs;
			AudioStats as;
			video_get_stats(&vs);
			audio_get_stats(&as);
			app_log("video in %llu out %llu shown %llu skip %llu rep %llu drop %llu err %llu lost %llu %ux%u decode %u/%u ms held %u.%02u%s late %llu margin %u ms %u.%02u Hz | audio buf %u/%u ms speed %+dppm underruns %llu dropped %llu",
					(unsigned long long)vs.frames_in, (unsigned long long)vs.frames_out,
					(unsigned long long)vs.frames_presented, (unsigned long long)vs.frames_skipped, (unsigned long long)vs.frames_repeated, (unsigned long long)vs.frames_dropped, (unsigned long long)vs.decode_errors,
					(unsigned long long)ctx.frames_lost, (unsigned)vs.width, (unsigned)vs.height,
					(unsigned)vs.decode_ms_avg, (unsigned)vs.decode_ms_max,
					(unsigned)(vs.decode_held_x100 / 100), (unsigned)(vs.decode_held_x100 % 100), vs.eop_marker ? " eop" : "", (unsigned long long)vs.flips_late,
					(unsigned)(vs.latch_margin_us / 1000), (unsigned)(vs.vsync_hz_x100 / 100), (unsigned)(vs.vsync_hz_x100 % 100),
					(unsigned)as.buffered_ms, (unsigned)as.target_ms, (int)as.speed_ppm, (unsigned long long)as.underruns, (unsigned long long)as.dropped_frames);
		}
	}

	video_display_set_overlay(NULL);
	main_watch = false;
	input.run = false;
	if(input_thread_ok)
		chiaki_thread_join(&input.thread, NULL);
	input_stop_rumble(); // never leave the motors running after a stream

	watchdog_arm(5000);
	app_log("Stopping the session");
	if(!ctx.quit || user_quit || app_exit || stalled)
		chiaki_session_stop(&session);
	chiaki_session_join(&session);
	app_log("Session thread finished");
	chiaki_session_fini(&session);
	watchdog_disarm();

	if(stalled && !app_exit)
	{
		dialog_wait_ok(true, "The connection to the %s was lost (no video for 10 seconds).\n\n"
				"Check the network cables of both consoles and try again.", host->ps5 ? "PS5" : "PS4");
	}

	if(ctx.quit && !user_quit && !app_exit && !was_connected && !last_attempt && quit_reason_retryable(ctx.quit_reason))
		retry = true;
	else if(ctx.quit && !user_quit && !app_exit && chiaki_quit_reason_is_error(ctx.quit_reason))
	{
		// plain explanation first, the technical reason (also in chiaki.log) last
		dialog_wait_ok(true, "%s\n\n(%s%s%s)", explain_quit(ctx.quit_reason, host->ps5, was_connected),
				chiaki_quit_reason_string(ctx.quit_reason), ctx.quit_reason_str[0] ? ": " : "", ctx.quit_reason_str);
	}

out_decoders:
	dialog_close();
	chiaki_opus_decoder_fini(&opus);
	video_decoder_fini();
out_dump:
	if(ctx.dump)
		fclose(ctx.dump);
	return retry;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	sysModuleLoad(SYSMODULE_NET);
	sysModuleLoad(SYSMODULE_NETCTL);
	sysModuleLoad(SYSMODULE_SYSUTIL);
	netInitialize();
	netCtlInit();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

	chiaki_mutex_init(&log_mutex, false);
	// keep the log of the previous run: it is the one that explains a crash or a hang
	remove(LOG_PREV_PATH);
	rename(LOG_PATH, LOG_PREV_PATH);
	log_file = fopen(LOG_PATH, "w");
	app_log("Chiaki PS3 %s starting", APP_VERSION);
	chiaki_thread_create(&watchdog_thread, watchdog_func, NULL);
	log_ip();

	static Config cfg; // large: keep it off the stack
	bool have_cfg = config_load(&cfg);
	chiaki_log_init(&app_log_ctx,
			cfg.verbose_log ? CHIAKI_LOG_ALL : (CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG)),
			chiaki_log_cb, NULL);
	app_log("Config: %s (%s), %d console(s)", cfg.path, have_cfg ? "loaded" : "new", cfg.host_count);

	if(!video_display_init(&app_log_ctx))
	{
		app_log("Display init failed");
		goto out;
	}
	video_display_set_background(0x101018);
	input_init();
	if(!audio_init(&app_log_ctx))
		app_log("Audio unavailable, continuing without sound");

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		dialog_wait_ok(true, "Chiaki init failed: %s", chiaki_error_string(err));
		goto out_av;
	}

	// Menu -> connect -> stream -> back to the menu.
	while(!app_exit)
	{
		int host_index = menu_shell_run(&cfg);
		if(host_index < 0 || app_exit)
			break;
		ConfigHost *host = &cfg.hosts[host_index];
		bool psn_available = config_has_psn(&cfg);
		bool direct_psn = psn_available && (host->use_psn || (!host->host[0] && !host->host_id[0]));
		bool try_psn = direct_psn;

		if(!direct_psn)
		{
			char addr_before[64];
			snprintf(addr_before, sizeof(addr_before), "%s", host->host);
			if(find_console(&cfg, host, !psn_available))
			{
				if(strcmp(addr_before, host->host) != 0 && !config_save(&cfg))
					app_log("Could not save the new address of %s", host->name);
				app_log("Using %s on LAN at %s", host->ps5 ? "PS5" : "PS4", host->host);
				for(int attempt = 1; attempt <= SESSION_ATTEMPTS && !app_exit; attempt++)
				{
					if(!run_session(&cfg, host, NULL, attempt == SESSION_ATTEMPTS))
						break;
					app_log("Connection failed at the start, trying again (%d of %d)", attempt + 1, SESSION_ATTEMPTS);
					dialog_status(host->ps5 ? "The PS5 did not answer yet, trying again..." : "The PS4 did not answer yet, trying again...");
					uint64_t until = chiaki_time_now_monotonic_ms() + 2000;
					while(chiaki_time_now_monotonic_ms() < until && keep_display_alive())
						;
				}
			}
			else if(!app_exit && psn_available)
			{
				app_log("Console not found on LAN, falling back to PSN (Internet) connection");
				try_psn = true;
			}
		}

		if(try_psn && !app_exit)
		{
			char psn_err[256] = {0};
			ChiakiHolepunchSession hp = psn_connect_console(&cfg, host, psn_err, sizeof(psn_err));
			dialog_close();
			if(!hp)
			{
				if(!app_exit)
					dialog_wait_ok(true, "%s", psn_err[0] ? psn_err : "PSN connection failed.");
				continue;
			}
			app_log("Using %s via PSN (Internet holepunch)", host->ps5 ? "PS5" : "PS4");
			run_session(&cfg, host, hp, true);
		}
	}

out_av:
	watchdog_arm(5000);
	audio_fini();
	input_fini();
	video_display_fini();
out:
	app_log("Exiting");
	if(log_file)
		fclose(log_file);
	netCtlTerm();
	netDeinitialize();
	return 0;
}
