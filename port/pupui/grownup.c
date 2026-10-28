/*
 * RufusOS Settings, and the sum gate that ends Kids mode (both buttons held 3 s).
 *
 * Menu: Wi-Fi (scan, connect with an on-screen keyboard, forget), Terminal (a shell
 * on a pty), Status (battery, memory, storage, brightness), Desktop (if installed),
 * Start in Kids (boot default), Android. The upper button goes up one level; the screen
 * turning off (timeout or power) returns to RufusOS (or the pups) and ends the shell.
 *
 * Wi-Fi talks to wpa_supplicant's control socket directly, so SSIDs and passwords
 * never pass through a shell. Passwords are not logged.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "ui.h"

#define DESKTOP "/usr/local/bin/rufus-desktop"
#define WPA_CTRL "/run/wpa_supplicant/wlan0"
#define PSY "/sys/class/power_supply/"
#define BL_MAX "/sys/class/backlight/backlight/max_brightness"

#define C_BG 0xff15181e
#define C_PANEL 0xff262b35
#define C_KEY 0xff3a404c
#define C_KEY2 0xff2c313b
#define C_ACCENT 0xff3d7cf0
#define C_RED 0xffc83c3c
#define C_TEXT 0xffffff
#define C_DIM 0x98a2b3

static enum { GATE, MENU, WIFI, WIFI_PASS, STATUS, TERM } screen;
static char msg[160];		/* one-line notice under the title */

/* ---------------------------------------------------------------- widgets */

int hit(int x, int y, int bx, int by, int bw, int bh)
{
	return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void text_mid(const struct font *f, int cx, int y, uint32_t c, const char *s)
{
	text(f, cx - text_width(f, s) / 2, y, c, s);
}

/* s cut to fit in w pixels (with ".." when cut). */
const char *fit(const struct font *f, const char *s, int w)
{
	static char buf[160];
	snprintf(buf, sizeof(buf), "%s", s);
	if (text_width(f, buf) <= w)
		return buf;
	/* Leave room for ".." and the terminator: buf can be full after the snprintf. */
	int n = strlen(buf);
	if (n > (int)sizeof(buf) - 3)
		n = sizeof(buf) - 3;
	for (; n > 0; n--) {
		strcpy(buf + n, "..");
		if (text_width(f, buf) <= w)
			break;
	}
	return buf;
}

void button(int x, int y, int w, int h, const char *label, uint32_t bg)
{
	fill_rrect(x, y, w, h, 12, bg);
	text_mid(&font_s, x + w / 2, y + (h - font_s.height) / 2, C_TEXT, label);
}

#define BACK_X 520
#define BACK_Y 6
#define BACK_W 110
#define BACK_H 44

static void title_bar(const char *title, int back)
{
	fill_rect(0, 0, W, 56, C_PANEL);
	text(&font_s, 20, 13, C_TEXT, fit(&font_s, title, back ? BACK_X - 30 : W - 40));
	if (back)
		button(BACK_X, BACK_Y, BACK_W, BACK_H, "Back", C_KEY);
}

/* A question with up to three answers over the current screen. */
static struct {
	int on, n;
	char text[160];
	const char *b[3];
	void (*fn)(int);
} dlg;

static void dialog(const char *text, int n, const char *b0, const char *b1, const char *b2, void (*fn)(int))
{
	dlg.on = 1;
	dlg.n = n;
	snprintf(dlg.text, sizeof(dlg.text), "%s", text);
	dlg.b[0] = b0;
	dlg.b[1] = b1;
	dlg.b[2] = b2;
	dlg.fn = fn;
}

static void dialog_draw(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			cv[y][x] = 0xff000000 | ((cv[y][x] >> 2) & 0x3f3f3f);
	fill_rrect(40, 120, 560, 240, 18, C_PANEL);
	text_mid(&font_s, W / 2, 160, C_TEXT, fit(&font_s, dlg.text, 520));
	int bw = (520 - (dlg.n - 1) * 16) / dlg.n;
	for (int i = 0; i < dlg.n; i++)
		button(60 + i * (bw + 16), 270, bw, 70, dlg.b[i], i == 0 ? C_ACCENT : C_KEY);
}

static void dialog_touch(int ev, int x, int y)
{
	if (ev != EV_UP)
		return;
	int bw = (520 - (dlg.n - 1) * 16) / dlg.n;
	for (int i = 0; i < dlg.n; i++)
		if (hit(x, y, 60 + i * (bw + 16), 270, bw, 70)) {
			dlg.on = 0;
			dlg.fn(i);
		}
}

/* ---------------------------------------------------------------- keyboard */

/*
 * Rows of keys, widths in half-key units (20 per row). \x01 shift, \x02 symbols,
 * \x03 more symbols, \x04 letters, \b delete, \n enter.
 */
static const char *const kb_rows[3][4] = {
	{ "qwertyuiop", "asdfghjkl'", "\x01zxcvbnm\b", "\x02, .\n" },
	{ "1234567890", "@#$%&*-+=/", "\x03!?():;\"\b", "\x04, .\n" },
	{ "~`|\\^_[]{}", "<>'\"!?@#$%", "\x02&*()-+=\b", "\x04, .\n" },
};
#define KB_KEY_H 48
#define KB_Y (H - 4 * KB_KEY_H)
static int kb_layer, kb_shift, kb_row = -1, kb_col;

static int kb_units(char c) { return c == ' ' ? 9 : c == '\n' ? 4 : (c < 32 || c == '\b') ? 3 : 2; }

static const char *kb_label(char c)
{
	static char s[2];
	switch (c) {
	case '\x01': return "Shift";
	case '\x02': return "123";
	case '\x03': return "#+=";
	case '\x04': return "abc";
	case '\b': return "Del";
	case '\n': return "Enter";
	case ' ': return "";
	}
	s[0] = kb_shift && c >= 'a' && c <= 'z' ? c - 32 : c;
	return s;
}

static void kb_draw(void)
{
	fill_rect(0, KB_Y, W, H - KB_Y, C_BG);
	for (int r = 0; r < 4; r++) {
		const char *row = kb_rows[kb_layer][r];
		for (int i = 0, u = 0; row[i]; u += kb_units(row[i]), i++) {
			char c = row[i];
			uint32_t bg = (r == kb_row && i == kb_col) ? C_ACCENT :
				      (c == '\x01' && kb_shift) ? C_ACCENT :
				      (c < 32 || c == '\b') ? C_KEY2 : C_KEY;
			int x = u * 32 + 3, y = KB_Y + r * KB_KEY_H + 3, w = kb_units(c) * 32 - 6;
			fill_rrect(x, y, w, KB_KEY_H - 6, 8, bg);
			const char *l = kb_label(c);
			const struct font *f = strlen(l) > 1 ? &font_t : &font_s;
			text_mid(f, x + w / 2, y + (KB_KEY_H - 6 - f->height) / 2, C_TEXT, l);
		}
	}
}

/* A key, once it is released: a character, '\b', '\n', or 0. */
static int kb_touch(int ev, int x, int y)
{
	if (ev == EV_DOWN) {
		kb_row = -1;
		if (y < KB_Y)
			return 0;
		int r = (y - KB_Y) / KB_KEY_H;
		const char *row = kb_rows[kb_layer][r];
		for (int i = 0, u = 0; row[i]; u += kb_units(row[i]), i++)
			if (x >= u * 32 && x < (u + kb_units(row[i])) * 32) {
				kb_row = r;
				kb_col = i;
			}
		return 0;
	}
	if (ev != EV_UP || kb_row < 0)
		return 0;
	char c = kb_rows[kb_layer][kb_row][kb_col];
	kb_row = -1;
	switch (c) {
	case '\x01': kb_shift = !kb_shift; return 0;
	case '\x02': kb_layer = 1; return 0;
	case '\x03': kb_layer = 2; return 0;
	case '\x04': kb_layer = 0; return 0;
	}
	if (kb_shift && c >= 'a' && c <= 'z') {
		c -= 32;
		kb_shift = 0;
	}
	return (unsigned char)c;
}

static void kb_reset(void)
{
	kb_layer = kb_shift = 0;
	kb_row = -1;
}

/* ---------------------------------------------------------------- the sum */

static int gate_a, gate_b, gate_tries;
static char gate_in[4];

#define PAD_X 340
#define PAD_Y 76
#define PAD_K 92
#define PAD_G 8
static const char *const pad[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "Del", "0", "OK" };

static void gate_new(void)
{
	gate_a = 6 + rnd(4);
	gate_b = 3 + rnd(7);
	gate_in[0] = 0;
}

static void gate_draw(void)
{
	char s[32];

	fill_rect(0, 0, W, H, C_BG);
	text(&font_s, 20, 20, C_TEXT, "Grown-ups only");
	snprintf(s, sizeof(s), "%d x %d =", gate_a, gate_b);
	text_mid(&font_m, 165, 140, C_TEXT, s);
	fill_rrect(85, 200, 160, 70, 12, C_PANEL);
	text_mid(&font_m, 165, 212, C_TEXT, gate_in);
	snprintf(s, sizeof(s), gate_tries == 1 ? "last try" : "%d tries", gate_tries);
	text_mid(&font_t, 165, 290, C_DIM, s);
	button(40, 390, 250, 70, "Cancel", C_KEY);
	for (int i = 0; i < 12; i++)
		button(PAD_X + (i % 3) * (PAD_K + PAD_G), PAD_Y + (i / 3) * (PAD_K + PAD_G), PAD_K, PAD_K,
		       pad[i], i == 11 ? C_ACCENT : C_KEY);
}

static void gate_touch(int ev, int x, int y)
{
	if (ev != EV_UP)
		return;
	if (hit(x, y, 40, 390, 250, 70)) {
		kid_home();
		return;
	}
	for (int i = 0; i < 12; i++) {
		if (!hit(x, y, PAD_X + (i % 3) * (PAD_K + PAD_G), PAD_Y + (i / 3) * (PAD_K + PAD_G), PAD_K, PAD_K))
			continue;
		size_t n = strlen(gate_in);
		if (i == 9) {
			if (n)
				gate_in[n - 1] = 0;
		} else if (i == 11) {
			if (atoi(gate_in) == gate_a * gate_b) {
				kids_unlock();
			} else if (--gate_tries == 0) {
				kid_home();
			} else {
				gate_new();
			}
		} else if (n < 3) {
			gate_in[n] = pad[i][0];
			gate_in[n + 1] = 0;
		}
	}
}

/* ---------------------------------------------------------------- Wi-Fi */

/* One request to wpa_supplicant's control socket; the reply in buf. */
static int wpa_cmd(const char *cmd, char *buf, size_t len)
{
	struct sockaddr_un local = { .sun_family = AF_UNIX }, dest = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0), n = -1;

	if (s < 0)
		return -1;
	snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/pupui-wpa-%d", (int)getpid());
	snprintf(dest.sun_path, sizeof(dest.sun_path), "%s", WPA_CTRL);
	unlink(local.sun_path);
	if (bind(s, (struct sockaddr *)&local, sizeof(local)) == 0 &&
	    connect(s, (struct sockaddr *)&dest, sizeof(dest)) == 0 &&
	    send(s, cmd, strlen(cmd), 0) >= 0) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		if (poll(&p, 1, 3000) > 0)
			n = recv(s, buf, len - 1, 0);
	}
	if (n >= 0)
		buf[n] = 0;
	close(s);
	unlink(local.sun_path);
	return n;
}

static int wpa_ok(const char *cmd)
{
	char r[64];
	return wpa_cmd(cmd, r, sizeof(r)) > 0 && !strncmp(r, "OK", 2);
}

/* SSIDs as wpa_supplicant prints them (printf-escaped), back to raw bytes. */
static int ssid_decode(const char *in, unsigned char *out, int max)
{
	int n = 0;
	while (*in && n < max) {
		if (*in != '\\') {
			out[n++] = *in++;
			continue;
		}
		in++;
		switch (*in) {
		case 'n': out[n++] = '\n'; in++; break;
		case 'r': out[n++] = '\r'; in++; break;
		case 't': out[n++] = '\t'; in++; break;
		case 'e': out[n++] = 27; in++; break;
		case 'x': {
			unsigned v;
			if (sscanf(in + 1, "%2x", &v) != 1)
				return -1;
			out[n++] = v;
			in += 3;
			break;
		}
		case 0: return -1;
		default: out[n++] = *in++; break;
		}
	}
	return n;
}

#define NET_MAX 32
static struct net {
	char ssid[100];		/* escaped, as wpa_supplicant prints it */
	int dbm, secure, sae, wep, saved_id;
} nets[NET_MAX];
static int net_n, net_off, scanning;
static long scan_t, wifi_t;

static struct {
	int id;
	char ssid[100];
} saved[NET_MAX];
static int saved_n;

static char wpa_state[32], cur_ssid[100], cur_ip[20];
static int cur_id = -1, wpa_up;

static struct {
	int id, fresh;		/* fresh: added now, removed again if it never connects */
	long t;
	char ssid[100];
} conn = { .id = -1 };

static char pass[64];
static int pass_show, pass_net;

static void wifi_status(void)
{
	char r[2048], *l, *save;

	wpa_up = wpa_cmd("STATUS", r, sizeof(r)) > 0;
	wpa_state[0] = cur_ssid[0] = cur_ip[0] = 0;
	cur_id = -1;
	for (l = strtok_r(r, "\n", &save); wpa_up && l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "wpa_state=", 10))
			snprintf(wpa_state, sizeof(wpa_state), "%s", l + 10);
		else if (!strncmp(l, "ssid=", 5))
			snprintf(cur_ssid, sizeof(cur_ssid), "%s", l + 5);
		else if (!strncmp(l, "ip_address=", 11))
			snprintf(cur_ip, sizeof(cur_ip), "%s", l + 11);
		else if (!strncmp(l, "id=", 3))
			cur_id = atoi(l + 3);
	}
	if (strcmp(wpa_state, "COMPLETED"))
		cur_ssid[0] = 0;

	static char list[4096];
	saved_n = 0;
	if (!wpa_up || wpa_cmd("LIST_NETWORKS", list, sizeof(list)) <= 0)
		return;
	l = strtok_r(list, "\n", &save);	/* header */
	while ((l = strtok_r(NULL, "\n", &save)) && saved_n < NET_MAX) {
		char *tab = strchr(l, '\t'), *end;
		if (!tab)
			continue;
		end = strchr(tab + 1, '\t');
		if (end)
			*end = 0;
		saved[saved_n].id = atoi(l);
		snprintf(saved[saved_n].ssid, sizeof(saved[0].ssid), "%s", tab + 1);
		saved_n++;
	}
	for (int i = 0; i < net_n; i++) {
		nets[i].saved_id = -1;
		for (int j = 0; j < saved_n; j++)
			if (!strcmp(nets[i].ssid, saved[j].ssid))
				nets[i].saved_id = saved[j].id;
	}
}

static void wifi_scan(long t)
{
	char r[64];
	if (wpa_cmd("SCAN", r, sizeof(r)) > 0 && (!strncmp(r, "OK", 2) || strstr(r, "BUSY"))) {
		scanning = 1;
		scan_t = t;
	}
}

static int net_cmp(const void *a, const void *b)
{
	return ((const struct net *)b)->dbm - ((const struct net *)a)->dbm;
}

/* bssid \t frequency \t signal \t flags \t ssid; one entry per SSID, strongest first. */
static void wifi_results(void)
{
	static char r[16384];
	char *l, *save;

	scanning = 0;
	if (wpa_cmd("SCAN_RESULTS", r, sizeof(r)) <= 0)
		return;
	net_n = 0;
	l = strtok_r(r, "\n", &save);
	while ((l = strtok_r(NULL, "\n", &save))) {
		char *f[5];
		int k = 0;
		for (char *p = l; k < 5 && p; k++) {
			f[k] = p;
			p = k < 4 ? strchr(p, '\t') : NULL;
			if (p)
				*p++ = 0;
		}
		if (k < 5 || !f[4][0])
			continue;	/* hidden network */
		int dbm = atoi(f[2]), j;
		for (j = 0; j < net_n && strcmp(nets[j].ssid, f[4]); j++)
			;
		if (j < net_n) {
			if (dbm > nets[j].dbm)
				nets[j].dbm = dbm;
			continue;
		}
		if (net_n == NET_MAX)
			continue;
		struct net *n = &nets[net_n++];
		snprintf(n->ssid, sizeof(n->ssid), "%s", f[4]);
		n->dbm = dbm;
		n->wep = strstr(f[3], "WEP") != NULL;
		n->sae = strstr(f[3], "SAE") && !strstr(f[3], "PSK");
		n->secure = n->wep || strstr(f[3], "WPA") || strstr(f[3], "RSN") || strstr(f[3], "SAE");
		n->saved_id = -1;
	}
	qsort(nets, net_n, sizeof(nets[0]), net_cmp);
	net_off = 0;
	wifi_status();	/* marks saved networks */
}

static void connect_id(int id, int fresh, const char *ssid, long t)
{
	char cmd[32];
	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	if (!wpa_ok(cmd)) {
		snprintf(msg, sizeof(msg), "wpa_supplicant refused the network");
		return;
	}
	conn.id = id;
	conn.fresh = fresh;
	conn.t = t;
	snprintf(conn.ssid, sizeof(conn.ssid), "%s", ssid);
	snprintf(msg, sizeof(msg), "Connecting to %s ...", ssid);
}

/* A new network: SSID as hex (any bytes), then the password or open. */
static void connect_new(struct net *n, const char *psk, long t)
{
	unsigned char raw[32];
	char r[32], cmd[160];
	int len = ssid_decode(n->ssid, raw, sizeof(raw)), id;

	if (len <= 0 || wpa_cmd("ADD_NETWORK", r, sizeof(r)) <= 0 || r[0] < '0' || r[0] > '9') {
		snprintf(msg, sizeof(msg), "Could not add the network");
		return;
	}
	id = atoi(r);
	int p = snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid ", id);
	for (int i = 0; i < len; i++)
		p += snprintf(cmd + p, sizeof(cmd) - p, "%02x", raw[i]);
	int ok = wpa_ok(cmd);
	if (ok && psk) {
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", id, psk);
		ok = wpa_ok(cmd);
		memset(cmd, 0, sizeof(cmd));
		if (ok && n->sae) {
			snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt SAE", id);
			ok = wpa_ok(cmd);
			snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ieee80211w 2", id);
			ok = ok && wpa_ok(cmd);
		}
	} else if (ok) {
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
		ok = wpa_ok(cmd);
	}
	if (!ok) {
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
		wpa_ok(cmd);
		snprintf(msg, sizeof(msg), "wpa_supplicant refused the settings");
		return;
	}
	connect_id(id, 1, n->ssid, t);
}

/* Called from adult_tick while a connection is pending. */
static void connect_check(long t)
{
	char cmd[32];

	if (conn.id < 0)
		return;
	if (!strcmp(wpa_state, "COMPLETED") && cur_id == conn.id) {
		/* SELECT_NETWORK disabled the others; keep them all for next time. */
		wpa_ok("ENABLE_NETWORK all");
		wpa_ok("SAVE_CONFIG");
		snprintf(msg, sizeof(msg), "Connected to %s", conn.ssid);
		conn.id = -1;
	} else if (t - conn.t > 25000) {
		if (conn.fresh) {
			snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", conn.id);
			wpa_ok(cmd);
		}
		wpa_ok("ENABLE_NETWORK all");
		snprintf(msg, sizeof(msg), "No connection to %s. Wrong password?", conn.ssid);
		conn.id = -1;
	}
}

static int dlg_net;

static void saved_choice(int b)
{
	char cmd[32];
	struct net *n = &nets[dlg_net];

	if (b == 0) {
		connect_id(n->saved_id, 0, n->ssid, now_ms());
	} else if (b == 1) {
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", n->saved_id);
		if (wpa_ok(cmd) && wpa_ok("SAVE_CONFIG"))
			snprintf(msg, sizeof(msg), "Forgot %s", n->ssid);
		wifi_status();
	}
}

#define ROW_Y 112
#define ROW_H 58
#define ROWS 6

static void signal_bars(int x, int y, int dbm)
{
	int bars = dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : dbm >= -85 ? 1 : 0;
	for (int i = 0; i < 4; i++)
		fill_rect(x + i * 9, y + 24 - i * 7, 6, 8 + i * 7, i < bars ? C_TEXT | 0xff000000 : 0xff4a505c);
}

static void wifi_draw(void)
{
	char s[160];

	fill_rect(0, 0, W, H, C_BG);
	title_bar("Wi-Fi", 1);
	button(BACK_X - 130, BACK_Y, 120, BACK_H, scanning ? "..." : "Scan", C_KEY);
	if (!wpa_up)
		snprintf(s, sizeof(s), "Wi-Fi service is not running");
	else if (msg[0])
		snprintf(s, sizeof(s), "%s", msg);
	else if (cur_ssid[0])
		snprintf(s, sizeof(s), "Connected: %s  %s", cur_ssid, cur_ip[0] ? cur_ip : "(no address yet)");
	else
		snprintf(s, sizeof(s), "Not connected (%s)", wpa_state[0] ? wpa_state : "?");
	text(&font_t, 20, 70, C_DIM, fit(&font_t, s, W - 40));

	if (!net_n)
		text(&font_t, 20, ROW_Y + 20, C_DIM, scanning ? "Scanning..." : "No networks found. Tap Scan.");
	for (int i = 0; i < ROWS && net_off + i < net_n; i++) {
		struct net *n = &nets[net_off + i];
		int y = ROW_Y + i * ROW_H, connected = cur_ssid[0] && !strcmp(cur_ssid, n->ssid);
		fill_rrect(10, y + 2, W - 20, ROW_H - 4, 10, connected ? 0xff1f3a6e : C_PANEL);
		text(&font_s, 24, y + (ROW_H - font_s.height) / 2, C_TEXT, fit(&font_s, n->ssid, 380));
		const char *tag = connected ? "connected" : n->saved_id >= 0 ? "saved" : n->secure ? "locked" : "open";
		text(&font_t, 430, y + (ROW_H - font_t.height) / 2, C_DIM, tag);
		signal_bars(570, y + 14, n->dbm);
	}
	if (net_n > ROWS) {
		snprintf(s, sizeof(s), "%d-%d of %d, drag to scroll", net_off + 1,
			 net_off + ROWS < net_n ? net_off + ROWS : net_n, net_n);
		text_mid(&font_t, W / 2, H - 26, C_DIM, s);
	}
}

static int drag_y, drag_off, dragging;

static void wifi_touch(int ev, int x, int y, long t)
{
	if (ev == EV_DOWN) {
		drag_y = y;
		drag_off = net_off;
		dragging = 0;
		return;
	}
	if (ev == EV_MOVE) {
		if (abs(y - drag_y) > 20)
			dragging = 1;
		if (dragging) {
			int off = drag_off - (y - drag_y) / ROW_H;
			int max = net_n > ROWS ? net_n - ROWS : 0;
			net_off = off < 0 ? 0 : off > max ? max : off;
		}
		return;
	}
	if (dragging)
		return;
	if (hit(x, y, BACK_X - 130, BACK_Y, 120, BACK_H)) {
		msg[0] = 0;
		wifi_scan(t);
		return;
	}
	if (y < ROW_Y || (y - ROW_Y) / ROW_H >= ROWS)
		return;
	int i = net_off + (y - ROW_Y) / ROW_H;
	if (i >= net_n)
		return;
	struct net *n = &nets[i];
	char q[140];
	dlg_net = i;
	msg[0] = 0;
	if (n->saved_id >= 0) {
		snprintf(q, sizeof(q), "%s", n->ssid);
		dialog(q, 3, "Connect", "Forget", "Cancel", saved_choice);
	} else if (n->wep) {
		snprintf(msg, sizeof(msg), "%s uses WEP, which is not supported", n->ssid);
	} else if (!n->secure) {
		connect_new(n, NULL, t);
	} else {
		pass_net = i;
		pass_show = 0;
		memset(pass, 0, sizeof(pass));
		kb_reset();
		screen = WIFI_PASS;
	}
}

static void pass_draw(void)
{
	char s[140], shown[64];

	fill_rect(0, 0, W, H, C_BG);
	snprintf(s, sizeof(s), "Password: %s", nets[pass_net].ssid);
	title_bar(s, 1);
	size_t n = strlen(pass);
	if (pass_show) {
		snprintf(shown, sizeof(shown), "%s", pass);
	} else {
		memset(shown, '*', n);
		shown[n] = 0;
	}
	fill_rrect(10, 66, 470, 60, 10, C_PANEL);
	/* The end of the password stays visible as it grows. */
	const char *v = shown;
	while (*v && text_width(&font_s, v) > 440)
		v++;
	text(&font_s, 24, 66 + (60 - font_s.height) / 2, C_TEXT, v);
	button(490, 66, 140, 60, pass_show ? "Hide" : "Show", C_KEY);
	snprintf(s, sizeof(s), n < 8 ? "%zu characters (at least 8)" : "%zu characters. Enter connects.", n);
	text(&font_t, 20, 140, C_DIM, s);
	memset(shown, 0, sizeof(shown));
	kb_draw();
}

static void pass_touch(int ev, int x, int y, long t)
{
	if (ev == EV_UP && hit(x, y, 490, 66, 140, 60)) {
		pass_show = !pass_show;
		return;
	}
	int c = kb_touch(ev, x, y);
	size_t n = strlen(pass);
	if (c == '\b') {
		if (n)
			pass[n - 1] = 0;
	} else if (c == '\n') {
		if (n < 8)
			return;
		connect_new(&nets[pass_net], pass, t);
		memset(pass, 0, sizeof(pass));
		screen = WIFI;
	} else if (c >= 32 && n < 63) {
		pass[n] = c;
	}
}

/* ---------------------------------------------------------------- status */

static char st_lines[8][200];

static long read_long(const char *path)
{
	FILE *f = fopen(path, "r");
	long v = -1;
	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	buf[0] = 0;
	if (f) {
		if (fgets(buf, len, f))
			buf[strcspn(buf, "\n")] = 0;
		fclose(f);
	}
}

static void status_read(void)
{
	char st[32], *line = NULL;
	size_t cap = 0;
	long avail = -1, total = -1;
	struct statvfs vfs;
	struct utsname u;

	read_str(PSY "battery/status", st, sizeof(st));
	snprintf(st_lines[0], sizeof(st_lines[0]), "Battery   %ld %%   %.2f V   %ld mA   %.1f C   %s",
		 read_long(PSY "battery/capacity"), read_long(PSY "battery/voltage_now") / 1e6,
		 read_long(PSY "battery/current_now") / 1000, read_long(PSY "battery/temp") / 10.0, st);
	snprintf(st_lines[1], sizeof(st_lines[1]), "Charger   %s",
		 read_long(PSY "mt6357-charger/online") == 1 ? "plugged in" : "not plugged in");
	snprintf(st_lines[2], sizeof(st_lines[2]), "Wi-Fi     %s  %s", cur_ssid[0] ? cur_ssid : "not connected", cur_ip);

	FILE *f = fopen("/proc/meminfo", "r");
	while (f && getline(&line, &cap, f) > 0) {
		sscanf(line, "MemTotal: %ld", &total);
		sscanf(line, "MemAvailable: %ld", &avail);
	}
	if (f)
		fclose(f);
	free(line);
	snprintf(st_lines[3], sizeof(st_lines[3]), "Memory    %.1f GB free of %.1f GB", avail / 1048576.0, total / 1048576.0);
	if (statvfs("/", &vfs) == 0)
		snprintf(st_lines[4], sizeof(st_lines[4]), "Storage   %llu MB free of %llu MB",
			 (unsigned long long)vfs.f_bavail * vfs.f_frsize >> 20,
			 (unsigned long long)vfs.f_blocks * vfs.f_frsize >> 20);
	long up = read_long("/proc/uptime");
	snprintf(st_lines[5], sizeof(st_lines[5]), "Uptime    %ld h %02ld min", up / 3600, up / 60 % 60);
	uname(&u);
	snprintf(st_lines[6], sizeof(st_lines[6]), "Linux     %s %s", u.release, u.machine);
}

#define SL_X 110
#define SL_W 420
#define SL_Y 400

static void status_draw(void)
{
	fill_rect(0, 0, W, H, C_BG);
	title_bar("Status", 1);
	for (int i = 0; i < 7; i++)
		text(&font_t, 20, 72 + i * 42, C_TEXT, fit(&font_t, st_lines[i], W - 40));
	int max = sysfs_read_int(BL_MAX);
	if (max <= 0)
		return;
	text(&font_t, 20, SL_Y - 34, C_DIM, "Brightness");
	button(20, SL_Y, 70, 60, "-", C_KEY);
	button(SL_X + SL_W + 20, SL_Y, 70, 60, "+", C_KEY);
	fill_rrect(SL_X, SL_Y + 20, SL_W, 20, 10, C_PANEL);
	fill_rrect(SL_X, SL_Y + 20, SL_W * saved_bl / max, 20, 10, C_ACCENT);
}

static void status_touch(int ev, int x, int y)
{
	int max = sysfs_read_int(BL_MAX), v = saved_bl;
	if (max <= 0 || (ev == EV_MOVE ? !hit(x, y, SL_X, SL_Y, SL_W, 60) : ev != EV_UP))
		return;
	if (hit(x, y, 20, SL_Y, 70, 60))
		v -= max / 16 > 0 ? max / 16 : 1;
	else if (hit(x, y, SL_X + SL_W + 20, SL_Y, 70, 60))
		v += max / 16 > 0 ? max / 16 : 1;
	else if (hit(x, y, SL_X, SL_Y, SL_W, 60))
		v = (x - SL_X) * max / SL_W;
	else
		return;
	saved_bl = v < 1 ? 1 : v > max ? max : v;	/* never 0: that looks like off */
	sysfs_write_int(BL, saved_bl);
}

/* ---------------------------------------------------------------- terminal */

/* A shell on a pty, drawn as a small VT100 subset (enough for busybox ash). */
#define T_MAXC 80
#define T_MAXR 16
#define T_EXTRA_Y (KB_Y - 40)
static char tscr[T_MAXR][T_MAXC];
static int tcols, trows, tx, ty, tfd = -1, tpid, tesc, tparam[4], tnp;
static int texited;

static const struct { const char *label, *seq; } t_extra[8] = {
	{ "Esc", "\033" }, { "Tab", "\t" }, { "^C", "\003" }, { "^D", "\004" },
	{ "Up", "\033[A" }, { "Down", "\033[B" }, { "Left", "\033[D" }, { "Right", "\033[C" },
};

static void t_clear(int r0, int c0, int r1, int c1)	/* inclusive start, exclusive end */
{
	for (int r = r0; r < r1; r++)
		for (int c = (r == r0 ? c0 : 0); c < (r == r1 - 1 ? c1 : tcols); c++)
			tscr[r][c] = ' ';
}

static void t_newline(void)
{
	if (++ty < trows)
		return;
	memmove(tscr[0], tscr[1], sizeof(tscr[0]) * (trows - 1));
	memset(tscr[trows - 1], ' ', sizeof(tscr[0]));
	ty = trows - 1;
}

static void t_put(unsigned char c)
{
	if (tx >= tcols) {	/* deferred wrap */
		tx = 0;
		t_newline();
	}
	tscr[ty][tx++] = c;
}

static void t_csi(char f)
{
	int a = tnp > 0 && tparam[0] ? tparam[0] : 1;
	char r[32];

	switch (f) {
	case 'A': ty = ty - a < 0 ? 0 : ty - a; break;
	case 'B': ty = ty + a >= trows ? trows - 1 : ty + a; break;
	case 'C': tx = tx + a >= tcols ? tcols - 1 : tx + a; break;
	case 'D': tx = tx - a < 0 ? 0 : tx - a; break;
	case 'G': tx = a - 1 < tcols ? a - 1 : tcols - 1; break;
	case 'H': case 'f':
		ty = (tnp > 0 && tparam[0] ? tparam[0] : 1) - 1;
		tx = (tnp > 1 && tparam[1] ? tparam[1] : 1) - 1;
		ty = ty >= trows ? trows - 1 : ty;
		tx = tx >= tcols ? tcols - 1 : tx;
		break;
	case 'J':
		if (tnp && tparam[0] == 2)
			t_clear(0, 0, trows, tcols);
		else
			t_clear(ty, tx < tcols ? tx : tcols, trows, tcols);
		break;
	case 'K':
		if (tnp && tparam[0] == 1)
			memset(tscr[ty], ' ', tx + 1 < tcols ? tx + 1 : tcols);
		else if (tnp && tparam[0] == 2)
			memset(tscr[ty], ' ', tcols);
		else if (tx < tcols)
			memset(tscr[ty] + tx, ' ', tcols - tx);
		break;
	case 'P':
		if (tx < tcols) {
			int n = a > tcols - tx ? tcols - tx : a;
			memmove(tscr[ty] + tx, tscr[ty] + tx + n, tcols - tx - n);
			memset(tscr[ty] + tcols - n, ' ', n);
		}
		break;
	case '@':
		if (tx < tcols) {
			int n = a > tcols - tx ? tcols - tx : a;
			memmove(tscr[ty] + tx + n, tscr[ty] + tx, tcols - tx - n);
			memset(tscr[ty] + tx, ' ', n);
		}
		break;
	case 'n':	/* cursor position report (busybox asks for it) */
		if (tnp && tparam[0] == 6) {
			int n = snprintf(r, sizeof(r), "\033[%d;%dR", ty + 1, (tx < tcols ? tx : tcols - 1) + 1);
			if (write(tfd, r, n) < 0)
				break;
		}
		break;
	}	/* colours, modes and the rest: ignored */
}

static void t_feed(const unsigned char *b, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char c = b[i];
		if (tesc == 1) {		/* after ESC */
			tesc = c == '[' ? 2 : c == ']' ? 3 : 0;
			tnp = 0;
			memset(tparam, 0, sizeof(tparam));
			continue;
		}
		if (tesc == 2) {		/* CSI parameters */
			if (c >= '0' && c <= '9') {
				if (!tnp)
					tnp = 1;
				tparam[tnp - 1] = tparam[tnp - 1] * 10 + c - '0';
			} else if (c == ';') {
				if (!tnp)
					tnp = 1;
				if (tnp < 4)
					tparam[tnp++] = 0;
			} else if (c >= 0x40) {
				t_csi(c);
				tesc = 0;
			}	/* '?' and other intermediates: skipped */
			continue;
		}
		if (tesc == 3) {		/* OSC: up to BEL or ESC */
			if (c == 7 || c == 27)
				tesc = c == 27;
			continue;
		}
		switch (c) {
		case 27: tesc = 1; break;
		case '\r': tx = 0; break;
		case '\n': t_newline(); break;
		case '\b': if (tx > 0) tx--; break;
		case '\t': tx = (tx / 8 + 1) * 8 < tcols ? (tx / 8 + 1) * 8 : tcols - 1; break;
		default: if (c >= 32 && c < 127) t_put(c); break;
		}
	}
}

static void t_kill(void)
{
	if (tfd >= 0)
		close(tfd);
	if (tpid > 0) {
		kill(-tpid, SIGHUP);
		kill(-tpid, SIGKILL);
		waitpid(tpid, NULL, 0);
	}
	tfd = -1;
	tpid = 0;
}

static void t_start(void)
{
	struct winsize ws;
	int cw = font_mono.g['M' - 32].adv;

	tcols = W / cw > T_MAXC ? T_MAXC : W / cw;
	trows = T_EXTRA_Y / font_mono.height > T_MAXR ? T_MAXR : T_EXTRA_Y / font_mono.height;
	memset(tscr, ' ', sizeof(tscr));
	tx = ty = tesc = texited = 0;

	tfd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (tfd < 0 || grantpt(tfd) || unlockpt(tfd)) {
		t_feed((const unsigned char *)"no pty", 6);
		t_kill();
		return;
	}
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = tcols;
	ws.ws_row = trows;
	ioctl(tfd, TIOCSWINSZ, &ws);
	const char *pts = ptsname(tfd);
	tpid = fork();
	if (tpid == 0) {
		setsid();
		int s = open(pts, O_RDWR);
		if (s < 0)
			_exit(127);
		ioctl(s, TIOCSCTTY, 0);
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (s > 2)
			close(s);
		for (int fd = 3; fd < 256; fd++)	/* pupui's input devices and framebuffer */
			close(fd);
		if (chdir("/root") && chdir("/"))
			_exit(127);
		setenv("TERM", "vt100", 1);
		setenv("HOME", "/root", 1);
		setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
		execl("/bin/sh", "-sh", (char *)NULL);
		_exit(127);
	}
	if (tpid < 0) {
		tpid = 0;
		t_kill();
		return;
	}
	fcntl(tfd, F_SETFL, O_NONBLOCK);
}

static void t_send(const char *s, size_t n)
{
	if (tfd >= 0 && write(tfd, s, n) < 0 && errno != EAGAIN)
		t_kill();
}

static void term_draw(void)
{
	int cw = font_mono.g['M' - 32].adv, chh = font_mono.height;
	char s[2] = { 0, 0 };

	fill_rect(0, 0, W, T_EXTRA_Y, 0xff0c0c0c);
	for (int r = 0; r < trows; r++)
		for (int c = 0; c < tcols; c++)
			if (tscr[r][c] != ' ') {
				s[0] = tscr[r][c];
				text(&font_mono, c * cw, r * chh, 0xd8d8d8, s);
			}
	if (tfd >= 0) {
		int cx = tx < tcols ? tx : tcols - 1;
		fill_rect(cx * cw, ty * chh + chh - 4, cw, 3, C_ACCENT);
	} else {
		text(&font_t, 20, T_EXTRA_Y - 30, C_DIM, "[shell ended: tap here for a new one]");
	}
	fill_rect(0, T_EXTRA_Y, W, 40, C_BG);
	for (int i = 0; i < 8; i++) {
		fill_rrect(i * 80 + 3, T_EXTRA_Y + 3, 74, 34, 8, C_KEY2);
		text_mid(&font_t, i * 80 + 40, T_EXTRA_Y + 3 + (34 - font_t.height) / 2, C_TEXT, t_extra[i].label);
	}
	kb_draw();
}

static void term_touch(int ev, int x, int y)
{
	if (tfd < 0) {
		if (ev == EV_UP && y < T_EXTRA_Y)
			t_start();
		return;
	}
	if (y >= T_EXTRA_Y && y < KB_Y) {
		if (ev == EV_UP)
			t_send(t_extra[x / 80].seq, strlen(t_extra[x / 80].seq));
		kb_row = -1;
		return;
	}
	int c = kb_touch(ev, x, y);
	char ch = c == '\n' ? '\r' : c == '\b' ? 0x7f : c;
	if (c)
		t_send(&ch, 1);
}

int adult_fd(void) { return screen == TERM ? tfd : -1; }

void adult_io(void)
{
	unsigned char b[4096];
	int n;
	while ((n = read(tfd, b, sizeof(b))) > 0)
		t_feed(b, n);
	if (n == 0 || (n < 0 && errno != EAGAIN)) {	/* EIO: the shell exited */
		t_kill();
		texited = 1;
	}
}

/* ---------------------------------------------------------------- menu */

static const char *const menu_items[6] = { "Wi-Fi", "Terminal", "Status", "Desktop", "Start in Kids", "Android" };

static void menu_rect(int i, int *x, int *y) { *x = 20 + (i % 2) * 310; *y = 72 + (i / 2) * 136; }

static void menu_draw(void)
{
	char sub[6][110];

	fill_rect(0, 0, W, H, C_BG);
	title_bar("Settings", 1);
	snprintf(sub[0], sizeof(sub[0]), "%s", !wpa_up ? "not running" : cur_ssid[0] ? cur_ssid : "not connected");
	snprintf(sub[1], sizeof(sub[1]), "%s", tfd >= 0 ? "shell running" : "root shell");
	snprintf(sub[2], sizeof(sub[2]), "battery %d %%", sysfs_read_int(PSY "battery/capacity"));
	snprintf(sub[3], sizeof(sub[3]), "%s", access(DESKTOP, X_OK) ? "not installed" : "Sxmo");
	snprintf(sub[4], sizeof(sub[4]), "%s", access(START_KIDS, F_OK) ? "off: starts in RufusOS" : "on: starts with the pups");
	snprintf(sub[5], sizeof(sub[5]), "reboot");
	for (int i = 0; i < 6; i++) {
		int x, y;
		menu_rect(i, &x, &y);
		fill_rrect(x, y, 290, 120, 16, i == 5 ? 0xff4a2a2a : C_PANEL);
		text_mid(&font_s, x + 145, y + 26, C_TEXT, menu_items[i]);
		text_mid(&font_t, x + 145, y + 70, C_DIM, fit(&font_t, sub[i], 270));
	}
	if (msg[0])
		text_mid(&font_t, W / 2, 448, C_DIM, fit(&font_t, msg, W - 20));
}

static void android_choice(int b)
{
	if (b == 0)
		bye();
}

static void menu_pick(int i, long t)
{
	msg[0] = 0;
	switch (i) {
	case 0:
		screen = WIFI;
		wifi_status();
		if (!net_n && wpa_up)
			wifi_scan(t);
		break;
	case 1:
		screen = TERM;
		kb_reset();
		if (tfd < 0)
			t_start();
		break;
	case 2:
		screen = STATUS;
		wifi_status();
		status_read();
		break;
	case 3:
		if (access(DESKTOP, X_OK)) {
			snprintf(msg, sizeof(msg), "No desktop installed (%s)", DESKTOP);
			break;
		}
		/* The desktop takes over the display; pupui comes back when it exits. */
		adult_leave();
		execl(DESKTOP, DESKTOP, (char *)NULL);
		screen = MENU;
		snprintf(msg, sizeof(msg), "Could not start the desktop");
		break;
	case 4:
		if (access(START_KIDS, F_OK) == 0)
			unlink(START_KIDS);
		else
			close(open(START_KIDS, O_CREAT | O_WRONLY, 0644));
		break;
	case 5:
		dialog("Reboot to Android?", 2, "Reboot", "Cancel", NULL, android_choice);
		break;
	}
}

static void menu_touch(int ev, int x, int y, long t)
{
	if (ev != EV_UP)
		return;
	for (int i = 0; i < 6; i++) {
		int bx, by;
		menu_rect(i, &bx, &by);
		if (hit(x, y, bx, by, 290, 120))
			menu_pick(i, t);
	}
}

/* ---------------------------------------------------------------- entry points */

void adult_enter(void)
{
	screen = GATE;
	gate_tries = 3;
	gate_new();
	msg[0] = 0;
	dlg.on = 0;
}

/* Settings from RufusOS: no sum; straight to the menu, the terminal or the desktop. */
void adult_open(int what)
{
	screen = MENU;
	msg[0] = 0;
	dlg.on = 0;
	if (what == SETTINGS_TERMINAL)
		menu_pick(1, now_ms());
	else if (what == SETTINGS_DESKTOP)
		menu_pick(3, now_ms());
}

void adult_leave(void)
{
	t_kill();
	memset(pass, 0, sizeof(pass));
	screen = GATE;
	dlg.on = 0;
}

void adult_back(void)
{
	if (dlg.on) {
		dlg.on = 0;
		return;
	}
	switch (screen) {
	case GATE: kid_home(); break;
	case MENU: settings_done(); break;
	case WIFI_PASS: memset(pass, 0, sizeof(pass)); screen = WIFI; break;
	default: screen = MENU; break;
	}
}

void adult_draw(void)
{
	switch (screen) {
	case GATE: gate_draw(); break;
	case MENU: menu_draw(); break;
	case WIFI: wifi_draw(); break;
	case WIFI_PASS: pass_draw(); break;
	case STATUS: status_draw(); break;
	case TERM: term_draw(); break;
	}
	if (dlg.on)
		dialog_draw();
}

void adult_touch(int ev, int x, int y, long t)
{
	if (dlg.on) {
		dialog_touch(ev, x, y);
		return;
	}
	if (screen != GATE && ev == EV_UP && hit(x, y, BACK_X, BACK_Y, BACK_W, BACK_H) &&
	    screen != TERM) {
		adult_back();
		return;
	}
	switch (screen) {
	case GATE: gate_touch(ev, x, y); break;
	case MENU: menu_touch(ev, x, y, t); break;
	case WIFI: wifi_touch(ev, x, y, t); break;
	case WIFI_PASS: pass_touch(ev, x, y, t); break;
	case STATUS: status_touch(ev, x, y); break;
	case TERM: term_touch(ev, x, y); break;
	}
}

int adult_tick(long t)
{
	int dirty = 0;

	if (screen == GATE || screen == TERM)
		return texited ? (texited = 0, 1) : 0;
	if (scanning && t - scan_t > 4000) {
		wifi_results();
		dirty = 1;
	}
	if (t - wifi_t > 2000 || (conn.id >= 0 && t - wifi_t > 1000)) {
		wifi_t = t;
		wifi_status();
		connect_check(t);
		if (screen == STATUS)
			status_read();
		dirty = 1;
	}
	return dirty;
}
