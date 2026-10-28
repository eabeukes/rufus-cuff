/*
 * RufusOS: the watch's main UI (pupui.c runs it; the pups are its Kids mode).
 *
 * Layout borrowed from AsteroidOS: a watch face, notifications above it (swipe up),
 * quick settings below (swipe down), apps beside it (swipe left). The upper button goes
 * back one level. Notifications come from rufus-ancs (the iPhone, over ANCS) through
 * NOTES_TXT; actions and pairing go back to it through ANCS_CTL.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#define NOTES_TXT "/run/rufus/notifications.txt"
#define ANCS_CTL "/run/rufus/ancs.ctl"
#define PASSKEY "/run/rufus/bt-passkey"
#define DND "/run/rufus/dnd"
#define BL_MAX "/sys/class/backlight/backlight/max_brightness"

#define C_BG 0xff0d0f13
#define C_CARD 0xff1d2129
#define C_CARD2 0xff2a303b
#define C_ACCENT 0xff3d7cf0
#define C_RED 0xffc83c3c
#define C_GREEN 0xff2e9d57
#define C_TEXT 0xffffff
#define C_DIM 0x8b95a5

#define SWIPE 70		/* pixels before a touch counts as a swipe */
#define BANNER_MS 5000
#define F_SILENT 1
#define F_PRE_EXISTING 4
#define F_POSITIVE 8
#define F_NEGATIVE 16

enum view { FACE, NOTES, NOTE, QUICK, APPS, PAIR, TIMER, STOPWATCH, FLASH };
static enum view view = FACE;

/* ---------------------------------------------------------------- notifications */

struct note {
	unsigned uid;
	long time;
	int flags;
	char cat[16], app[48], title[160], msg[400], pos[24], neg[24];
};

#define NOTE_MAX 50
static struct note notes[NOTE_MAX];
static int note_n, phone, note_sel = -1, note_scroll;
static long seen_time;		/* newest notification time when the list was last opened */
static long newest_time;	/* newest time loaded so far: newer ones are new */
static struct note banner;
static long banner_until;
static struct stat notes_st;	/* NOTES_TXT as last read (rewritten by rename: inode, mtime) */
static int notes_loaded;

static void field(char *dst, size_t n, char **p)
{
	char *end = strchr(*p, 0x1f);
	size_t len = end ? (size_t)(end - *p) : strlen(*p);

	snprintf(dst, n, "%.*s", (int)len, *p);
	*p = end ? end + 1 : *p + len;
}

/* Reread NOTES_TXT; returns 1 when a new (not silent, not old) notification arrived. */
static int notes_load(void)
{
	FILE *f = fopen(NOTES_TXT, "r");
	char line[1200], num[24];
	int fresh = 0;
	long top = newest_time;

	note_n = 0;
	phone = 0;
	if (!f)
		return 0;
	if (fgets(line, sizeof(line), f))
		phone = !strncmp(line, "connected 1", 11);
	while (note_n < NOTE_MAX && fgets(line, sizeof(line), f)) {
		struct note *n = &notes[note_n];
		char *p = line;

		line[strcspn(line, "\n")] = 0;
		field(num, sizeof(num), &p);
		n->uid = strtoul(num, NULL, 10);
		field(num, sizeof(num), &p);
		n->time = atol(num);
		field(n->cat, sizeof(n->cat), &p);
		field(num, sizeof(num), &p);
		n->flags = atoi(num);
		field(n->app, sizeof(n->app), &p);
		field(n->title, sizeof(n->title), &p);
		field(n->msg, sizeof(n->msg), &p);
		field(n->pos, sizeof(n->pos), &p);
		field(n->neg, sizeof(n->neg), &p);
		if (n->time > newest_time && !(n->flags & (F_SILENT | F_PRE_EXISTING)) && !fresh) {
			banner = *n;
			fresh = 1;
		}
		if (n->time > top)
			top = n->time;
		note_n++;
	}
	fclose(f);
	newest_time = top;
	if (note_sel >= note_n)
		note_sel = -1;
	return fresh;
}

static int is_unread(const struct note *n)
{
	return n->time > seen_time && !(n->flags & F_PRE_EXISTING);
}

static int unread(void)
{
	int k = 0;
	for (int i = 0; i < note_n; i++)
		k += is_unread(&notes[i]);
	return k;
}

static void ctl(const char *cmd)
{
	/* rufus-ancs keeps the FIFO open for reading, so this never blocks while it runs. */
	int fd = open(ANCS_CTL, O_WRONLY | O_NONBLOCK);
	if (fd >= 0) {
		if (write(fd, cmd, strlen(cmd)) < 0)
			perror("ancs.ctl");
		close(fd);
	}
}

/* The motor for ms milliseconds, without waiting (a double fork leaves no zombie). */
static void vibrate(int ms)
{
	char s[16];
	pid_t pid = fork();

	snprintf(s, sizeof(s), "%d", ms);
	if (pid == 0) {
		if (fork() == 0)
			execl("/usr/local/bin/vibrate", "vibrate", s, (char *)NULL);
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* ---------------------------------------------------------------- drawing helpers */

static void ago(char *s, size_t n, long when)
{
	long d = time(NULL) - when;
	if (d < 60)
		snprintf(s, n, "now");
	else if (d < 3600)
		snprintf(s, n, "%ld min", d / 60);
	else if (d < 86400)
		snprintf(s, n, "%ld h", d / 3600);
	else
		snprintf(s, n, "%ld d", d / 86400);
}

/* Word-wrapped text in a box; returns the y below the last line drawn. */
static int wrap(const struct font *f, int x, int y, int w, int ymax, uint32_t c, const char *s)
{
	char line[200];
	while (*s && y + f->height <= ymax) {
		int n = 0, cut = 0;
		while (s[n] && n < (int)sizeof(line) - 1) {
			line[n] = s[n];
			line[n + 1] = 0;
			if (text_width(f, line) > w)
				break;
			if (s[n] == ' ')
				cut = n;
			n++;
		}
		if (s[n] && n < (int)sizeof(line) - 1 && cut)
			n = cut;
		snprintf(line, sizeof(line), "%.*s", n, s);
		if (y + 2 * f->height > ymax && s[n])
			snprintf(line, sizeof(line), "%s", fit(f, s, w));
		text(f, x, y, c, line);
		y += f->height;
		s += n;
		while (*s == ' ')
			s++;
	}
	return y;
}

static void chevron(int cx, int y, int up)
{
	for (int i = 0; i < 12; i++)
		fill_rect(cx - i, y + (up ? i : 11 - i), 2 * i + 1, 2, 0xff3a4150);
}

static void top_bar(const char *title)
{
	fill_rect(0, 0, W, 56, C_CARD);
	text(&font_s, 20, 13, C_TEXT, title);
}

static void draw_banner(long t)
{
	if (t >= banner_until || view == FLASH)
		return;
	fill_rrect(10, 8, W - 20, 96, 18, C_CARD2);
	text(&font_t, 28, 16, C_DIM, fit(&font_t, banner.app, W - 60));
	text(&font_s, 28, 42, C_TEXT, fit(&font_s, banner.title[0] ? banner.title : banner.msg, W - 60));
	text(&font_t, 28, 74, C_DIM, fit(&font_t, banner.title[0] ? banner.msg : "", W - 60));
}

/* ---------------------------------------------------------------- face */

static void draw_face(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char s[64];
	int pct = battery_pct();

	localtime_r(&now, &tm);
	fill_rect(0, 0, W, H, C_BG);
	if (tm.tm_year + 1900 < 2026)
		snprintf(s, sizeof(s), "--:--");
	else
		snprintf(s, sizeof(s), "%d:%02d", tm.tm_hour, tm.tm_min);
	text_mid(&font_xl, W / 2, 70, C_TEXT, s);
	strftime(s, sizeof(s), "%A %e %B", &tm);
	text_mid(&font_s, W / 2, 262, C_DIM, tm.tm_year + 1900 < 2026 ? "clock not set" : s);

	/* status line: phone, battery */
	snprintf(s, sizeof(s), "%s   %s%d %%", phone ? "iPhone" : "no phone", plugged ? "charging " : "", pct);
	text_mid(&font_t, W / 2, 316, phone ? 0x6fb3ff : C_DIM, s);

	int k = unread();
	if (k) {
		snprintf(s, sizeof(s), "%d new", k);
		int w = text_width(&font_s, s) + 40;
		fill_rrect(W / 2 - w / 2, 366, w, 48, 24, C_ACCENT);
		text_mid(&font_s, W / 2, 374, C_TEXT, s);
	}
	chevron(W / 2, 14, 1);
	chevron(W / 2, H - 28, 0);
	for (int i = 0; i < 12; i++)	/* apps: "<" at the right edge */
		fill_rect(W - 34 + i, H / 2 - i, 2, 2 * i + 1, 0xff3a4150);
}

/* ---------------------------------------------------------------- notifications list */

#define CARD_H 104
#define LIST_Y 64

static void draw_notes(void)
{
	char s[24];

	fill_rect(0, 0, W, H, C_BG);
	if (!note_n) {
		top_bar("Notifications");
		text_mid(&font_s, W / 2, 190, C_DIM, "No notifications");
		text_mid(&font_t, W / 2, 236, C_DIM, phone ? "iPhone connected" :
			 "iPhone not connected (Quick settings: Pair)");
		return;
	}
	for (int i = 0; i < note_n; i++) {
		int y = LIST_Y + i * (CARD_H + 8) - note_scroll;
		if (y > H || y + CARD_H < 56)
			continue;
		struct note *n = &notes[i];
		fill_rrect(12, y, W - 24, CARD_H, 16, is_unread(n) ? C_CARD2 : C_CARD);
		ago(s, sizeof(s), n->time);
		text(&font_t, 30, y + 10, C_DIM, fit(&font_t, n->app, W - 170));
		text(&font_t, W - 30 - text_width(&font_t, s), y + 10, C_DIM, s);
		text(&font_s, 30, y + 38, C_TEXT, fit(&font_s, n->title[0] ? n->title : n->msg, W - 60));
		text(&font_t, 30, y + 72, C_DIM, fit(&font_t, n->title[0] ? n->msg : "", W - 60));
	}
	top_bar("Notifications");
}

/* Detail: the whole text and the phone's own action buttons (e.g. Clear; Answer/Decline). */
static void note_buttons(const struct note *n, int *bx, int *bw, const char **lb, int *nb)
{
	*nb = 0;
	if (n->flags & F_NEGATIVE)
		lb[(*nb)++] = n->neg[0] ? n->neg : "Clear";
	if (n->flags & F_POSITIVE)
		lb[(*nb)++] = n->pos[0] ? n->pos : "OK";
	for (int i = 0; i < *nb; i++) {
		bw[i] = (W - 40 - (*nb - 1) * 16) / *nb;
		bx[i] = 20 + i * (bw[i] + 16);
	}
}

static void draw_note(void)
{
	const struct note *n = &notes[note_sel];
	int bx[2], bw[2], nb;
	const char *lb[2];
	char s[24];

	fill_rect(0, 0, W, H, C_BG);
	ago(s, sizeof(s), n->time);
	text(&font_t, 20, 14, C_DIM, fit(&font_t, n->app, W - 140));
	text(&font_t, W - 20 - text_width(&font_t, s), 14, C_DIM, s);
	int y = wrap(&font_s, 20, 48, W - 40, 150, C_TEXT, n->title);
	wrap(&font_t, 20, y + 8, W - 40, 384, C_TEXT, n->msg);
	note_buttons(n, bx, bw, lb, &nb);
	for (int i = 0; i < nb; i++)
		button(bx[i], 396, bw[i], 72, lb[i],
		       !strcmp(n->cat, "incoming-call") ? (i == nb - 1 && nb == 2 ? C_GREEN : C_RED) :
		       (i == 0 && (n->flags & F_NEGATIVE)) ? C_CARD2 : C_ACCENT);
}

/* ---------------------------------------------------------------- quick settings */

static int bl_max(void)
{
	int m = sysfs_read_int(BL_MAX);
	return m > 0 ? m : 16;
}

static void draw_quick(void)
{
	char s[48];
	int dnd = access(DND, F_OK) == 0;

	fill_rect(0, 0, W, H, C_BG);
	top_bar("Quick settings");
	/* brightness */
	text(&font_t, 24, 76, C_DIM, "Brightness");
	button(24, 108, 110, 80, "-", C_CARD2);
	button(W - 134, 108, 110, 80, "+", C_CARD2);
	fill_rrect(150, 136, W - 300, 24, 12, C_CARD);
	fill_rrect(150, 136, (W - 300) * saved_bl / bl_max(), 24, 12, C_ACCENT);
	/* toggles */
	button(24, 220, 286, 100, dnd ? "Quiet: on" : "Quiet: off", dnd ? C_ACCENT : C_CARD2);
	button(330, 220, 286, 100, "Pair iPhone", C_CARD2);
	snprintf(s, sizeof(s), "%s, battery %d %%", phone ? "iPhone connected" : "no phone", battery_pct());
	text_mid(&font_t, W / 2, 356, C_DIM, s);
	button(24, 392, 592, 76, "Flashlight", C_CARD2);
}

static void set_brightness(int v)
{
	int m = bl_max();
	saved_bl = v < 1 ? 1 : v > m ? m : v;
	sysfs_write_int(BL, saved_bl);
}

/* ---------------------------------------------------------------- pairing */

static long pair_until;

static void draw_pair(long t)
{
	char key[16] = "", s[64];
	FILE *f = fopen(PASSKEY, "r");

	if (f) {
		if (!fgets(key, sizeof(key), f))
			key[0] = 0;
		fclose(f);
		key[strcspn(key, "\n")] = 0;
	}
	fill_rect(0, 0, W, H, C_BG);
	top_bar("Pair iPhone");
	if (key[0]) {
		text_mid(&font_t, W / 2, 96, C_DIM, "The iPhone should show this code:");
		text_mid(&font_l, W / 2, 140, C_TEXT, key);
		text_mid(&font_t, W / 2, 250, C_DIM, "If it matches, tap Pair on the iPhone, then turn on");
		text_mid(&font_t, W / 2, 280, C_DIM, "Share System Notifications for Rufus Cuff.");
	} else if (t < pair_until) {
		text_mid(&font_t, W / 2, 96, C_TEXT, "Pairing is open. On the iPhone:");
		text_mid(&font_t, W / 2, 140, C_DIM, "open nRF Connect (App Store), scan,");
		text_mid(&font_t, W / 2, 170, C_DIM, "and connect to \"Rufus Cuff\".");
		text_mid(&font_t, W / 2, 214, C_DIM, "Already paired iPhones reconnect by themselves.");
	} else {
		text_mid(&font_t, W / 2, 140, C_DIM, "Pairing has closed.");
	}
	if (t < pair_until) {
		snprintf(s, sizeof(s), "closes in %ld s", (pair_until - t) / 1000);
		text_mid(&font_t, W / 2, 340, C_DIM, s);
	}
	button(W / 2 - 150, 390, 300, 72, phone ? "Done (connected)" : "Done", phone ? C_GREEN : C_CARD2);
}

/* ---------------------------------------------------------------- apps grid */

struct app { const char *name; const struct image *icon; };
static const struct app apps[] = {
	{ "Timer", &icon_timer }, { "Stopwatch", &icon_stopwatch }, { "Flashlight", &icon_flashlight },
	{ "Kids", &icon_kids }, { "Settings", &icon_settings }, { "Terminal", &icon_terminal },
	{ "Desktop", &icon_desktop },
};
#define APP_N (int)(sizeof(apps) / sizeof(apps[0]))

static void app_pos(int i, int *x, int *y)
{
	*x = 30 + (i % 4) * 150;
	*y = 70 + (i / 4) * 190;
}

static void draw_apps(void)
{
	fill_rect(0, 0, W, H, C_BG);
	for (int i = 0; i < APP_N; i++) {
		int x, y;
		app_pos(i, &x, &y);
		blit(apps[i].icon, x + 8, y);
		text_mid(&font_t, x + 60, y + 114, C_TEXT, apps[i].name);
	}
}

/* ---------------------------------------------------------------- timer */

static long timer_set = 5 * 60000, timer_end, timer_left = 5 * 60000;
static int timer_run, timer_ring;

static void mmss(char *s, size_t n, long ms, int tenths)
{
	long sec = ms / 1000;
	if (tenths)
		snprintf(s, n, "%ld:%02ld.%ld", sec / 60, sec % 60, ms / 100 % 10);
	else
		snprintf(s, n, "%ld:%02ld", sec / 60, sec % 60);
}

static const int presets[5] = { 1, 3, 5, 10, 15 };

static void draw_timer(long t)
{
	char s[24];
	long left = timer_run ? timer_end - t : timer_left;

	fill_rect(0, 0, W, H, timer_ring ? 0xff5a1d1d : C_BG);
	mmss(s, sizeof(s), left < 0 ? 0 : left + 999, 0);
	text_mid(&font_xl, W / 2, 30, C_TEXT, timer_ring ? "0:00" : s);
	if (timer_ring) {
		text_mid(&font_m, W / 2, 230, C_TEXT, "Time's up!");
		button(W / 2 - 150, 380, 300, 80, "Stop", C_RED);
		return;
	}
	for (int i = 0; i < 5; i++) {
		snprintf(s, sizeof(s), "%d min", presets[i]);
		button(20 + i * 122, 230, 110, 70, s, timer_set == presets[i] * 60000L ? C_ACCENT : C_CARD2);
	}
	button(20, 380, 290, 80, timer_run ? "Pause" : "Start", timer_run ? C_CARD2 : C_GREEN);
	button(330, 380, 290, 80, "Reset", C_CARD2);
}

static void timer_touch(int x, int y, long t)
{
	if (timer_ring) {
		if (hit(x, y, W / 2 - 150, 380, 300, 80))
			timer_ring = 0, timer_left = timer_set;
		return;
	}
	for (int i = 0; i < 5; i++)
		if (hit(x, y, 20 + i * 122, 230, 110, 70) && !timer_run)
			timer_set = timer_left = presets[i] * 60000L;
	if (hit(x, y, 20, 380, 290, 80)) {
		if (timer_run)
			timer_left = timer_end - t;
		else
			timer_end = t + timer_left;
		timer_run = !timer_run;
	}
	if (hit(x, y, 330, 380, 290, 80))
		timer_run = 0, timer_left = timer_set;
}

/* ---------------------------------------------------------------- stopwatch */

static long sw_start, sw_acc, laps[4];
static int sw_run, lap_n;

static void draw_stopwatch(long t)
{
	char s[24];
	long el = sw_acc + (sw_run ? t - sw_start : 0);

	fill_rect(0, 0, W, H, C_BG);
	mmss(s, sizeof(s), el, 1);
	text_mid(&font_l, W / 2, 50, C_TEXT, s);
	for (int i = 0; i < lap_n; i++) {
		char l[40];
		mmss(s, sizeof(s), laps[i], 1);
		snprintf(l, sizeof(l), "Lap %d   %s", i + 1, s);
		text_mid(&font_t, W / 2, 170 + i * 44, C_DIM, l);
	}
	button(20, 380, 290, 80, sw_run ? "Stop" : "Start", sw_run ? C_RED : C_GREEN);
	button(330, 380, 290, 80, sw_run ? "Lap" : "Reset", C_CARD2);
}

static void stopwatch_touch(int x, int y, long t)
{
	if (hit(x, y, 20, 380, 290, 80)) {
		if (sw_run)
			sw_acc += t - sw_start;
		else
			sw_start = t;
		sw_run = !sw_run;
	} else if (hit(x, y, 330, 380, 290, 80)) {
		if (sw_run) {
			if (lap_n == 4)
				memmove(laps, laps + 1, 3 * sizeof(laps[0])), lap_n = 3;
			laps[lap_n++] = sw_acc + t - sw_start;
		} else {
			sw_acc = 0;
			lap_n = 0;
		}
	}
}

/* ---------------------------------------------------------------- navigation */

static int flash_saved;

static void go(enum view v)
{
	if (view == FLASH && v != FLASH)
		set_brightness(flash_saved);
	if (v == FLASH) {
		flash_saved = saved_bl;
		sysfs_write_int(BL, bl_max());
	}
	if (v == NOTES) {
		note_scroll = 0;
		seen_time = newest_time;
	}
	view = v;
}

void rufus_enter(void)
{
	go(FACE);
}

void rufus_back(void)
{
	switch (view) {
	case NOTE: go(NOTES); break;
	case FLASH: go(QUICK); break;
	case PAIR: go(QUICK); break;
	case TIMER: case STOPWATCH: go(APPS); break;
	default: go(FACE); break;
	}
}

static void open_app(int i)
{
	switch (i) {
	case 0: go(TIMER); break;
	case 1: go(STOPWATCH); break;
	case 2: go(FLASH); break;
	case 3: kids_enter(); break;
	case 4: settings_open(SETTINGS_MENU); break;
	case 5: settings_open(SETTINGS_TERMINAL); break;
	case 6: settings_open(SETTINGS_DESKTOP); break;
	}
}

static int down_x, down_y, scroll0, dragging;

void rufus_touch(int ev, int x, int y, long t)
{
	if (ev == EV_DOWN) {
		down_x = x, down_y = y, scroll0 = note_scroll, dragging = 0;
		return;
	}
	int dx = x - down_x, dy = y - down_y;
	if (ev == EV_MOVE) {
		if (view == NOTES && (dragging || abs(dy) > 20)) {
			int max = note_n * (CARD_H + 8) - (H - LIST_Y);
			dragging = 1;
			note_scroll = scroll0 - dy;
			if (note_scroll > max)
				note_scroll = max;
			if (note_scroll < 0)
				note_scroll = 0;
		}
		return;
	}
	/* EV_UP: a swipe or a tap */
	if (dragging)
		return;
	if (t < banner_until && down_y < 104 && abs(dx) < SWIPE && abs(dy) < SWIPE) {
		banner_until = 0;	/* tap the banner: open the list */
		go(NOTES);
		return;
	}
	int swipe = abs(dx) > SWIPE || abs(dy) > SWIPE;
	int horiz = abs(dx) > abs(dy);
	switch (view) {
	case FACE:
		if (swipe && !horiz)
			go(dy < 0 ? NOTES : QUICK);
		else if (swipe && dx < 0)
			go(APPS);
		else if (!swipe && y > 360 && unread())
			go(NOTES);
		break;
	case NOTES:
		if (swipe && horiz && dx > 0) {
			go(FACE);
			break;
		}
		if (swipe)
			break;
		for (int i = 0; i < note_n; i++) {
			int cy = LIST_Y + i * (CARD_H + 8) - note_scroll;
			if (y >= 56 && hit(x, y, 12, cy, W - 24, CARD_H)) {
				note_sel = i;
				go(NOTE);
			}
		}
		break;
	case NOTE: {
		int bx[2], bw[2], nb;
		const char *lb[2];
		if (swipe) {
			if (horiz && dx > 0)
				go(NOTES);
			break;
		}
		note_buttons(&notes[note_sel], bx, bw, lb, &nb);
		for (int i = 0; i < nb; i++) {
			if (!hit(x, y, bx[i], 396, bw[i], 72))
				continue;
			char cmd[32];
			int negative = i == 0 && (notes[note_sel].flags & F_NEGATIVE);
			snprintf(cmd, sizeof(cmd), "%s %u\n", negative ? "negative" : "positive", notes[note_sel].uid);
			ctl(cmd);
			go(NOTES);
		}
		break;
	}
	case QUICK:
		if (swipe && !horiz && dy < 0) {
			go(FACE);
			break;
		}
		if (swipe)
			break;
		if (hit(x, y, 24, 108, 110, 80))
			set_brightness(saved_bl - 1);
		else if (hit(x, y, W - 134, 108, 110, 80))
			set_brightness(saved_bl + 1);
		else if (hit(x, y, 24, 220, 286, 100)) {
			if (access(DND, F_OK) == 0)
				unlink(DND);
			else
				close(open(DND, O_CREAT | O_WRONLY, 0600));
		} else if (hit(x, y, 330, 220, 286, 100)) {
			unlink(PASSKEY);
			ctl("pair\n");
			pair_until = t + 120000;
			go(PAIR);
		} else if (hit(x, y, 24, 392, 592, 76))
			go(FLASH);
		break;
	case PAIR:
		if (!swipe && hit(x, y, W / 2 - 150, 390, 300, 72))
			go(QUICK);
		break;
	case APPS:
		if (swipe && horiz && dx > 0) {
			go(FACE);
			break;
		}
		for (int i = 0; i < APP_N && !swipe; i++) {
			int ax, ay;
			app_pos(i, &ax, &ay);
			if (hit(x, y, ax, ay, 120, 150))
				open_app(i);
		}
		break;
	case TIMER:
		if (!swipe)
			timer_touch(x, y, t);
		break;
	case STOPWATCH:
		if (!swipe)
			stopwatch_touch(x, y, t);
		break;
	case FLASH:
		go(QUICK);	/* any tap ends it */
		break;
	}
}

void rufus_draw(long t)
{
	switch (view) {
	case FACE: draw_face(); break;
	case NOTES: draw_notes(); break;
	case NOTE: draw_note(); break;
	case QUICK: draw_quick(); break;
	case PAIR: draw_pair(t); break;
	case APPS: draw_apps(); break;
	case TIMER: draw_timer(t); break;
	case STOPWATCH: draw_stopwatch(t); break;
	case FLASH: fill_rect(0, 0, W, H, 0xffffffff); break;
	}
	draw_banner(t);
}

/* Redraw often while something moves (timer, stopwatch, pairing countdown). */
int rufus_animating(void)
{
	return (view == TIMER && timer_run) || (view == STOPWATCH && sw_run) || view == PAIR;
}

/* Background work, called on every loop: RUFUS_REDRAW and/or RUFUS_WAKE (turn the screen on). */
int rufus_tick(long t)
{
	static long last_check, last_minute, ring_buzz;
	struct stat st;
	int r = 0;

	if (t - last_check >= 500) {
		last_check = t;
		if (stat(NOTES_TXT, &st) == 0 && (st.st_ino != notes_st.st_ino ||
		    st.st_mtim.tv_sec != notes_st.st_mtim.tv_sec ||
		    st.st_mtim.tv_nsec != notes_st.st_mtim.tv_nsec)) {
			int first = !notes_loaded;
			notes_st = st;
			notes_loaded = 1;
			if (notes_load() && !first && access(DND, F_OK) != 0) {
				banner_until = t + BANNER_MS;
				r |= RUFUS_WAKE;
			}
			r |= RUFUS_REDRAW;
		}
		if (view == NOTE && note_sel < 0)
			go(NOTES);
	}
	if (timer_run && t >= timer_end) {
		timer_run = 0;
		timer_ring = 1;
		ring_buzz = 0;
		go(TIMER);
		r |= RUFUS_WAKE | RUFUS_REDRAW;
	}
	if (timer_ring && t - ring_buzz > 1500) {
		ring_buzz = t;
		vibrate(600);
	}
	if (banner_until && t >= banner_until) {
		banner_until = 0;
		r |= RUFUS_REDRAW;
	}
	if (t / 60000 != last_minute) {	/* the face's clock */
		last_minute = t / 60000;
		r |= RUFUS_REDRAW;
	}
	return r;
}
