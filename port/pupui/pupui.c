/*
 * pupui: "Pup Rescue HQ", a kids' launcher and games for the Rufus Cuff (Linux).
 *
 * Draws on /dev/fb0 (480x640 portrait XRGB, panel mounted sideways) as a 640x480
 * landscape canvas: canvas (x, y) -> framebuffer column y, row 639 - x, so the picture is
 * upright with the watch's buttons on the right. Touch arrives already in canvas
 * coordinates (edt-ft5x06 with inverted-x + swapped-x-y in the DT).
 *
 * Buttons: upper button (KEY_BACK; worn buttons-right) = home; hold it 3 s = reboot (back to Android).
 *          power (KEY_POWER, short press) = screen off/on.
 *          hold both 3 s = grown-up mode (grownup.c), after a sum.
 * Games: Pup Pop (tap the pups), Badge Match (memory), Paint, Pup Parade.
 * All built-in artwork is original (gen_assets.py); theme.c can replace it.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/reboot.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

uint32_t cv[H][W];			/* landscape canvas */
static uint32_t paint_layer[H][W];	/* Paint game's picture */
static volatile uint32_t *fb;
static int fb_sw;			/* framebuffer stride in pixels */

/* ---------------------------------------------------------------- time, misc */

long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int rnd(int n) { return rand() % n; }

/* ---------------------------------------------------------------- drawing */

static uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
	if (a >= 255)
		return src | 0xff000000;
	if (a == 0)
		return dst;
	unsigned rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a)) >> 8;
	unsigned g = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a)) >> 8;
	return 0xff000000 | (rb & 0xff00ff) | (g & 0x00ff00);
}

void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
			cv[j][i] = c;
}

void fill_circle(int cx, int cy, int r, uint32_t c)
{
	for (int j = -r; j <= r; j++) {
		int y = cy + j;
		if (y < 0 || y >= H)
			continue;
		int dx = (int)sqrt((double)(r * r - j * j));
		for (int x = cx - dx; x <= cx + dx; x++)
			if (x >= 0 && x < W)
				cv[y][x] = c;
	}
}

void fill_rrect(int x, int y, int w, int h, int r, uint32_t c)
{
	fill_rect(x + r, y, w - 2 * r, h, c);
	fill_rect(x, y + r, r, h - 2 * r, c);
	fill_rect(x + w - r, y + r, r, h - 2 * r, c);
	fill_circle(x + r, y + r, r, c);
	fill_circle(x + w - r - 1, y + r, r, c);
	fill_circle(x + r, y + h - r - 1, r, c);
	fill_circle(x + w - r - 1, y + h - r - 1, r, c);
}

/* Draw an image, cut off at row ymax (exclusive). */
static void blit_above(const struct image *im, int x, int y, int ymax)
{
	for (int j = 0; j < im->h; j++) {
		int cy = y + j;
		if (cy < 0 || cy >= H || cy >= ymax)
			continue;
		const uint32_t *s = im->px + j * im->w;
		for (int i = 0; i < im->w; i++) {
			int cx = x + i;
			if (cx >= 0 && cx < W)
				cv[cy][cx] = blend(cv[cy][cx], s[i] & 0xffffff, s[i] >> 24);
		}
	}
}

void blit(const struct image *im, int x, int y) { blit_above(im, x, y, H); }

int text_width(const struct font *f, const char *s)
{
	int w = 0;
	for (; *s; s++)
		if (*s >= 32 && *s < 127)
			w += f->g[*s - 32].adv;
	return w;
}

void text(const struct font *f, int x, int y, uint32_t c, const char *s)
{
	for (; *s; s++) {
		if (*s < 32 || *s >= 127)
			continue;
		const struct glyph *g = &f->g[*s - 32];
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++) {
				int px = x + g->x + i, py = y + g->y + j;
				if (px >= 0 && px < W && py >= 0 && py < H)
					cv[py][px] = blend(cv[py][px], c, g->a[j * g->w + i]);
			}
		x += g->adv;
	}
}

/* Centred text with a soft drop shadow, readable on any background. */
void text_c(const struct font *f, int cx, int y, uint32_t c, const char *s)
{
	int x = cx - text_width(f, s) / 2;
	text(f, x + 3, y + 3, 0x000000, s);
	text(f, x, y, c, s);
}

static void sky_and_grass(void)
{
	for (int y = 0; y < H; y++) {
		uint32_t c = y < 300 ? (0xff000000 | ((90 + y / 3) << 16) | ((170 + y / 6) << 8) | 255)
				     : (y < 310 ? 0xff5cb043 : 0xff6cc450);
		for (int x = 0; x < W; x++)
			cv[y][x] = c;
	}
}

static void star(int cx, int cy, int r, uint32_t c)
{
	/* five-pointed star by scanning a polygon test */
	double px[10], py[10];
	for (int i = 0; i < 10; i++) {
		double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45 : r;
		px[i] = cx + rr * cos(a);
		py[i] = cy + rr * sin(a);
	}
	for (int y = cy - r; y <= cy + r; y++)
		for (int x = cx - r; x <= cx + r; x++) {
			int in = 0;
			for (int i = 0, j = 9; i < 10; j = i++)
				if ((py[i] > y) != (py[j] > y) &&
				    x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i])
					in = !in;
			if (in && x >= 0 && x < W && y >= 0 && y < H)
				cv[y][x] = c;
		}
}

static void home_button(void)
{
	fill_circle(42, 42, 32, 0xffffffff);
	fill_circle(42, 42, 28, 0xffe22626);
	/* little house */
	for (int j = 0; j < 16; j++)
		fill_rect(42 - j, 28 + j, 2 * j + 1, 1, 0xffffffff);
	fill_rect(32, 44, 21, 14, 0xffffffff);
	fill_rect(39, 49, 7, 9, 0xffe22626);
}

static int in_home_button(int x, int y) { return (x - 42) * (x - 42) + (y - 42) * (y - 42) < 40 * 40; }

/* ---------------------------------------------------------------- framebuffer */

void present(void)
{
	for (int x = 0; x < W; x++) {
		volatile uint32_t *row = fb + (W - 1 - x) * fb_sw;
		for (int y = 0; y < H; y++)
			row[y] = cv[y][x];
	}
}

static int fb_open(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;

	if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		perror("fb0");
		return -1;
	}
	if (var.xres != H || var.yres != W || var.bits_per_pixel != 32) {
		fprintf(stderr, "unexpected fb %ux%u@%u\n", var.xres, var.yres, var.bits_per_pixel);
		return -1;
	}
	fb_sw = fix.line_length / 4;
	fb = mmap(NULL, fix.line_length * var.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap fb0");
		return -1;
	}
	/* Stop fbcon from drawing kernel messages over us. */
	int tty = open("/dev/tty0", O_RDWR);
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_GRAPHICS);
	return 0;
}

/* ---------------------------------------------------------------- system bits */

int sysfs_read_int(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = -1;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

void sysfs_write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

#define SCREEN_TIMEOUT_MS 60000

/* The PMIC's AUXADC among the IIO devices (the accelerometer is one too). */
static int adc_read_bat(void)
{
	char path[80], name[16];

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%d/name", i);
		FILE *f = fopen(path, "r");
		int ok = f && fgets(name, sizeof(name), f) && !strcmp(name, "MT6357\n");
		if (f)
			fclose(f);
		if (ok) {
			snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%d/in_resistance0_raw", i);
			return sysfs_read_int(path);
		}
	}
	return -1;
}

#define PSY "/sys/class/power_supply/"
static int plugged;	/* charger connected: draw the bolt */

/* The fuel gauge's capacity; without it, a rough estimate from the battery voltage. */
static int battery_pct(void)
{
	static const int mv_tab[11] = {3400, 3600, 3700, 3750, 3790, 3830, 3870, 3920, 3980, 4060, 4200};
	int pct = sysfs_read_int(PSY "battery/capacity");

	plugged = sysfs_read_int(PSY "mt6357-charger/online") == 1;
	if (pct >= 0)
		return pct;
	int raw = adc_read_bat();
	if (raw < 0)
		return -1;
	int mv = (int)(raw * 0.164794921);	/* in_resistance0_scale */
	if (mv <= mv_tab[0])
		return 0;
	for (int i = 1; i < 11; i++)
		if (mv < mv_tab[i])
			return (i - 1) * 10 + (mv - mv_tab[i - 1]) * 10 / (mv_tab[i] - mv_tab[i - 1]);
	return 100;
}

static void battery_icon(int x, int y, int pct)
{
	uint32_t c = pct < 0 ? 0xff888888 : pct < 20 ? 0xffe22626 : pct < 50 ? 0xfff7941d : 0xff34a848;
	fill_rrect(x, y, 70, 34, 6, 0xffffffff);
	fill_rect(x + 70, y + 10, 6, 14, 0xffffffff);
	fill_rect(x + 4, y + 4, 62, 26, 0xff303030);
	if (pct > 0)
		fill_rect(x + 4, y + 4, 62 * pct / 100, 26, c);
	if (plugged) {	/* lightning bolt */
		for (int j = 0; j < 13; j++)
			fill_rect(x + 38 - j / 2, y + 4 + j, 7, 1, 0xffffd600);
		fill_rect(x + 29, y + 16, 16, 3, 0xffffd600);
		for (int j = 0; j < 12; j++)
			fill_rect(x + 32 - j / 2 + 6, y + 18 + j, 7, 1, 0xffffd600);
	}
}

/* The time in the lookout tower's window, once the clock has been set (RTC + offset, NTP). */
static void clock_in_tower(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char s[8];

	localtime_r(&now, &tm);
	if (tm.tm_year + 1900 < 2026)
		return;
	snprintf(s, sizeof(s), "%d:%02d", tm.tm_hour, tm.tm_min);
	text_c(&font_s, 525, 149, 0x2654c8, s);
}

/* ---------------------------------------------------------------- input */

static struct pollfd in_fds[9];	/* input devices, then the terminal */
static int in_n;
static int touch_x, touch_y, touch_down;

static void input_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) && in_n < 8) {
		char p[300];
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);	/* not kept by the desktop */
		if (fd >= 0) {
			in_fds[in_n].fd = fd;
			in_fds[in_n].events = POLLIN;
			in_n++;
		}
	}
	if (d)
		closedir(d);
}

/* ---------------------------------------------------------------- scenes */

enum scene { HOME, POP, MATCH, PAINT, PARADE, ADULT };
static enum scene scene = HOME;
static int battery = -1;

/* Home: four tiles. */
static const struct { const char *label; int pup; } tiles[4] = {
	{ "Pup Pop", 0 }, { "Match", 1 }, { "Paint", 2 }, { "Parade", 3 },
};

static void tile_rect(int i, int *x, int *y)
{
	*x = 24 + (i % 2) * 222;
	*y = 92 + (i / 2) * 192;
}

static void draw_home(void)
{
	blit(home_img, 0, 0);
	text_c(&font_m, 230, 22, 0xffffff, "Pup Rescue HQ");
	battery_icon(546, 18, battery);
	clock_in_tower();
	for (int i = 0; i < 4; i++) {
		int x, y;
		tile_rect(i, &x, &y);
		fill_rrect(x + 4, y + 6, 200, 172, 22, 0xff000000 | 0x30303030);
		fill_rrect(x, y, 200, 172, 22, 0xff000000 | pup_team[tiles[i].pup]);
		fill_rrect(x + 6, y + 6, 188, 118, 16, 0xffffffff);
		blit(pups_small[tiles[i].pup], x + 52, y + 16);
		text_c(&font_s, x + 100, y + 132, 0xffffff, tiles[i].label);
	}
}

/* Pup Pop: tap the pup before it hides. */
#define POP_ROUND_MS 40000
static struct { int hole, pup, score; long shown, until, start, next; int over, plus_x, plus_y; long plus_t; } pop;

static void hole_pos(int i, int *cx, int *cy) { *cx = 130 + (i % 3) * 190; *cy = 262 + (i / 3) * 124; }

static void pop_start(void)
{
	memset(&pop, 0, sizeof(pop));
	pop.hole = -1;
	pop.start = now_ms();
	pop.next = pop.start + 800;
}

static void pop_tick(long t)
{
	if (pop.over)
		return;
	if (t - pop.start > POP_ROUND_MS) {
		pop.over = 1;
		pop.hole = -1;
		return;
	}
	if (pop.hole >= 0 && t > pop.until) {
		pop.hole = -1;
		pop.next = t + 300 + rnd(500);
	}
	if (pop.hole < 0 && t > pop.next) {
		pop.hole = rnd(6);
		pop.pup = rnd(6);
		pop.shown = t;
		pop.until = t + 1600 - (t - pop.start) * 700 / POP_ROUND_MS;	/* speeds up */
	}
}

static void draw_pop(long t)
{
	sky_and_grass();
	home_button();
	char s[32];
	snprintf(s, sizeof(s), "Pups: %d", pop.score);
	text_c(&font_m, 330, 18, 0xffffff, s);
	long left = POP_ROUND_MS - (t - pop.start);
	if (left < 0)
		left = 0;
	fill_rrect(470, 30, 150, 22, 10, 0xff303030);
	int bar = (int)(150 * left / POP_ROUND_MS);
	if (bar > 20)
		fill_rrect(470, 30, bar, 22, 10, 0xffffd600);
	for (int i = 0; i < 6; i++) {
		int cx, cy;
		hole_pos(i, &cx, &cy);
		for (int j = 0; j < 28; j++) {	/* hole: dark ellipse, rim at cy + 54 */
			int half = (int)(80 * sqrt(1.0 - (double)(j - 14) * (j - 14) / 196.0));
			fill_rect(cx - half, cy + 40 + j, 2 * half, 1, 0xff4a3020);
		}
		if (i == pop.hole) {	/* the pup climbs out of the hole */
			int rise = (int)((t - pop.shown) / 3);
			if (rise > 90)
				rise = 90;
			blit_above(pups_small[pop.pup], cx - 48, cy + 54 - rise, cy + 54);
		}
	}
	if (t - pop.plus_t < 600)
		text_c(&font_m, pop.plus_x, pop.plus_y - (int)((t - pop.plus_t) / 10), 0xffd600, "+1");
	if (pop.over) {
		fill_rrect(110, 120, 420, 240, 30, 0xffffffff);
		text_c(&font_m, 320, 140, 0x2654c8, "Great job!");
		snprintf(s, sizeof(s), "%d pups rescued", pop.score);
		text_c(&font_s, 320, 200, 0x303030, s);
		int stars = pop.score >= 25 ? 3 : pop.score >= 12 ? 2 : 1;
		for (int i = 0; i < 3; i++)
			star(220 + i * 100, 285, 38, i < stars ? 0xffffd600 : 0xffd0d0d0);
	}
}

static void pop_touch(int x, int y, long t)
{
	if (pop.over) {
		pop_start();
		return;
	}
	if (pop.hole < 0)
		return;
	int cx, cy;
	hole_pos(pop.hole, &cx, &cy);
	if (abs(x - cx) < 85 && y > cy - 110 && y < cy + 70) {
		pop.score++;
		pop.plus_x = cx;
		pop.plus_y = cy - 120;
		pop.plus_t = t;
		pop.hole = -1;
		pop.next = t + 250;
	}
}

/* Badge Match: memory with 6 pairs. */
static struct { int card[12], up[12], done[12], first, second, pairs; long flip_back; } mt;

static void match_start(void)
{
	memset(&mt, 0, sizeof(mt));
	for (int i = 0; i < 12; i++)
		mt.card[i] = i / 2;
	for (int i = 11; i > 0; i--) {
		int j = rnd(i + 1), k = mt.card[i];
		mt.card[i] = mt.card[j];
		mt.card[j] = k;
	}
	mt.first = mt.second = -1;
}

static void card_pos(int i, int *x, int *y) { *x = 92 + (i % 4) * 122; *y = 92 + (i / 4) * 128; }

static void draw_match(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			cv[y][x] = 0xff260000 | (0x54 - y / 16) << 8 | (0xc8 - y / 8);	/* blue, darker down */
	home_button();
	text_c(&font_m, 340, 18, 0xffffff, "Badge Match");
	for (int i = 0; i < 12; i++) {
		int x, y;
		card_pos(i, &x, &y);
		if (mt.up[i] || mt.done[i]) {
			fill_rrect(x, y, 112, 118, 14, mt.done[i] ? 0xfffff3b0 : 0xffffffff);
			blit(badges[mt.card[i]], x + 6, y + 9);
		} else {
			fill_rrect(x, y, 112, 118, 14, 0xffffd600);
			fill_rrect(x + 6, y + 6, 100, 106, 10, 0xffe22626);
			/* paw print */
			fill_circle(x + 56, y + 70, 18, 0xffffffff);
			fill_circle(x + 36, y + 46, 8, 0xffffffff);
			fill_circle(x + 50, y + 38, 8, 0xffffffff);
			fill_circle(x + 64, y + 38, 8, 0xffffffff);
			fill_circle(x + 78, y + 46, 8, 0xffffffff);
		}
	}
	if (mt.pairs == 6) {
		fill_rrect(110, 140, 420, 200, 30, 0xffffffff);
		text_c(&font_m, 320, 160, 0x34a848, "You did it!");
		for (int i = 0; i < 3; i++)
			star(220 + i * 100, 270, 38, 0xffffd600);
	}
}

static void match_tick(long t)
{
	if (mt.second >= 0 && t > mt.flip_back) {
		mt.up[mt.first] = mt.up[mt.second] = 0;
		mt.first = mt.second = -1;
	}
}

static void match_touch(int x, int y, long t)
{
	if (mt.pairs == 6) {
		match_start();
		return;
	}
	if (mt.second >= 0)
		return;	/* wait for the mismatch to flip back */
	for (int i = 0; i < 12; i++) {
		int cx, cy;
		card_pos(i, &cx, &cy);
		if (x < cx || x > cx + 112 || y < cy || y > cy + 118 || mt.up[i] || mt.done[i])
			continue;
		mt.up[i] = 1;
		if (mt.first < 0) {
			mt.first = i;
		} else if (mt.card[mt.first] == mt.card[i]) {
			mt.done[mt.first] = mt.done[i] = 1;
			mt.pairs++;
			mt.first = -1;
		} else {
			mt.second = i;
			mt.flip_back = t + 900;
		}
		break;
	}
}

/* Paint: seven colours, an eraser and a "new" button. */
static const uint32_t paint_cols[7] = {0xffe22626, 0xfff7941d, 0xffffd600, 0xff34a848,
				       0xff2654c8, 0xffec5ca8, 0xff6b3d1e};
#define PAINT_TOP 92
#define ERASER 7
#define NEW_X 556
static struct { int col, lx, ly, drawing; } pt;

static void paint_clear(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			paint_layer[y][x] = 0xffffffff;
}

static void paint_dot(int cx, int cy, uint32_t c)
{
	for (int j = -12; j <= 12; j++)
		for (int i = -12; i <= 12; i++)
			if (i * i + j * j <= 144) {
				int x = cx + i, y = cy + j;
				if (x >= 0 && x < W && y >= PAINT_TOP && y < H)
					paint_layer[y][x] = c;
			}
}

static void paint_line(int x0, int y0, int x1, int y1, uint32_t c)
{
	int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
	steps = steps / 4 + 1;
	for (int s = 0; s <= steps; s++)
		paint_dot(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, c);
}

static int pot_x(int i) { return 120 + i * 58; }

static void draw_paint(void)
{
	memcpy(cv, paint_layer, sizeof(cv));
	fill_rect(0, 0, W, PAINT_TOP - 4, 0xffe8eef8);
	home_button();
	for (int i = 0; i < 8; i++) {
		int cx = pot_x(i), sel = pt.col == i;
		uint32_t c = i < ERASER ? paint_cols[i] : 0xffffffff;
		fill_circle(cx, 44, sel ? 29 : 24, sel ? 0xff303030 : 0xffb0b0b0);
		fill_circle(cx, 44, sel ? 25 : 21, c);
		if (i == ERASER)
			fill_rrect(cx - 13, 36, 26, 16, 4, 0xffec5ca8);
	}
	fill_rrect(NEW_X, 18, 76, 52, 12, 0xff303030);
	text_c(&font_s, NEW_X + 38, 30, 0xffffff, "new");
}

static void paint_touch(int ev, int x, int y)
{
	if (ev == EV_DOWN && y < PAINT_TOP) {
		for (int i = 0; i < 8; i++)
			if (abs(x - pot_x(i)) < 28)
				pt.col = i;
		if (x >= NEW_X)
			paint_clear();
		pt.drawing = 0;
		return;
	}
	if (ev == EV_UP) {
		pt.drawing = 0;
		return;
	}
	uint32_t c = pt.col == ERASER ? 0xffffffff : paint_cols[pt.col];
	if (!pt.drawing || ev == EV_DOWN)
		paint_dot(x, y, c);
	else
		paint_line(pt.lx, pt.ly, x, y, c);
	pt.lx = x;
	pt.ly = y;
	pt.drawing = 1;
}

/* Pup Parade: the pups march along; tap one and it jumps and woofs. */
static struct { double x[6], jump_v[6], jump_y[6]; long woof_until[6]; long last; } pa;

static void parade_start(void)
{
	memset(&pa, 0, sizeof(pa));
	for (int i = 0; i < 6; i++)
		pa.x[i] = -110.0 * i;
	pa.last = now_ms();
}

static void parade_tick(long t)
{
	double dt = (t - pa.last) / 1000.0;
	pa.last = t;
	for (int i = 0; i < 6; i++) {
		pa.x[i] += 90 * dt;
		if (pa.x[i] > W + 60)
			pa.x[i] -= 6 * 110 + 60;
		if (pa.jump_y[i] > 0 || pa.jump_v[i] > 0) {
			pa.jump_y[i] += pa.jump_v[i] * dt;
			pa.jump_v[i] -= 900 * dt;
			if (pa.jump_y[i] <= 0) {
				pa.jump_y[i] = 0;
				pa.jump_v[i] = 0;
			}
		}
	}
}

static void draw_parade(long t)
{
	sky_and_grass();
	home_button();
	text_c(&font_m, 340, 18, 0xffffff, "Pup Parade");
	text_c(&font_m, 320, 410, 0xffffff, "Tap a pup!");
	for (int i = 0; i < 6; i++) {
		int bob = (int)(6 * fabs(sin((t / 180.0) + i)));
		int x = (int)pa.x[i] - 48, y = 300 - 96 + 20 - bob - (int)pa.jump_y[i];
		for (int j = 0; j < 12; j++) {	/* shadow */
			int half = (int)(36 * sqrt(1.0 - (j - 5.5) * (j - 5.5) / 36.0));
			fill_rect(x + 48 - half, 312 + j, 2 * half, 1, 0xff4c9a38);
		}
		blit(pups_small[i], x, y);
		if (t < pa.woof_until[i]) {
			fill_rrect(x - 10, y - 62, 140, 56, 20, 0xffffffff);
			text_c(&font_s, x + 60, y - 54, 0x000000 | pup_team[i], "Woof!");
			text_c(&font_s, x + 60, 360, 0xffffff, pup_names[i]);
		}
	}
}

static void parade_touch(int x, int y, long t)
{
	for (int i = 0; i < 6; i++) {
		int px = (int)pa.x[i];
		if (abs(x - px) < 55 && y > 150 && y < 340 && pa.jump_y[i] == 0) {
			pa.jump_v[i] = 340;
			pa.jump_y[i] = 0.1;
			pa.woof_until[i] = t + 1200;
		}
	}
}


/* ---------------------------------------------------------------- main loop */

static void enter(enum scene s)
{
	scene = s;
	if (s == POP)
		pop_start();
	else if (s == MATCH)
		match_start();
	else if (s == PARADE)
		parade_start();
	else if (s == ADULT)
		adult_enter();
}

void kid_home(void)
{
	adult_leave();
	enter(HOME);
}

/* Back to Android: a clean reboot. */
void bye(void)
{
	fill_rect(0, 0, W, H, 0xff000000);
	text_c(&font_m, W / 2, 200, 0xffffff, "Bye bye!");
	present();
	sync();
	/*
	 * On the root filesystem, rufus-android picks the partition Android is in and
	 * signals busybox init (PID 1), which runs the OpenRC shutdown (unmount, charger
	 * off) and reboots. In the bare initramfs PID 1 is a shell script, so reboot
	 * directly: into "recovery" unless LK started us from there (atag,boot: u32
	 * size, tag, mode, little-endian; mode 2 = recovery, Android is then in "boot").
	 */
	if (access("/etc/inittab", F_OK) == 0 && system("/usr/local/bin/rufus-android") == 0)
		pause();
	unsigned char atag[12] = { 0 };
	FILE *f = fopen("/proc/device-tree/chosen/atag,boot", "rb");
	if (f) {
		if (fread(atag, 1, sizeof(atag), f) != sizeof(atag))
			atag[8] = 0;
		fclose(f);
	}
	if (atag[8] != 2)
		syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			LINUX_REBOOT_CMD_RESTART2, "recovery");
	reboot(LINUX_REBOOT_CMD_RESTART);
}

static void handle_touch(int ev, int x, int y, long t)
{
	if (scene != HOME && scene != ADULT && ev == EV_DOWN && in_home_button(x, y)) {
		enter(HOME);
		return;
	}
	switch (scene) {
	case HOME:
		if (ev != EV_UP)
			return;
		for (int i = 0; i < 4; i++) {
			int tx, ty;
			tile_rect(i, &tx, &ty);
			if (x >= tx && x < tx + 200 && y >= ty && y < ty + 172)
				enter((enum scene)(POP + i));
		}
		break;
	case POP:
		if (ev == EV_DOWN)
			pop_touch(x, y, t);
		break;
	case MATCH:
		if (ev == EV_DOWN)
			match_touch(x, y, t);
		break;
	case PAINT:
		paint_touch(ev, x, y);
		break;
	case PARADE:
		if (ev == EV_DOWN)
			parade_touch(x, y, t);
		break;
	case ADULT:
		adult_touch(ev, x, y, t);
		break;
	}
}

int saved_bl;

int main(void)
{
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	if (fb_open())
		return 1;
	input_open();
	theme_load();
	paint_clear();
	battery = battery_pct();

	int screen_on = 1;
	long back_down = 0, power_down = 0, last_batt = now_ms(), last_frame = 0, last_input = now_ms();
	int dirty = 1, ev_pending = EV_NONE, combo = 0;

	saved_bl = sysfs_read_int(BL);
	if (saved_bl <= 0)
		saved_bl = 12;

	for (;;) {
		long t = now_ms();
		int animating = scene == POP || scene == PARADE || (scene == MATCH && mt.second >= 0);
		int timeout = !screen_on ? 1000 : animating ? 33 : 500;
		int n = in_n;

		/* The grown-up terminal's output wakes us too (slot after the input devices). */
		if (scene == ADULT && adult_fd() >= 0) {
			in_fds[n].fd = adult_fd();
			in_fds[n].events = POLLIN;
			in_fds[n++].revents = 0;
		}
		if (poll(in_fds, n, timeout) > 0) {
			if (n > in_n && in_fds[in_n].revents) {
				adult_io();
				dirty = 1;
			}
			for (int i = 0; i < in_n; i++) {
				struct input_event e;
				while (read(in_fds[i].fd, &e, sizeof(e)) == sizeof(e)) {
					if (e.type == EV_KEY)
						last_input = t;
					if (e.type == EV_KEY && e.code == KEY_POWER) {
						/*
						 * Power toggles the screen on release, so that holding
						 * it with the second button (grown-up mode) does not.
						 */
						if (e.value == 1) {
							power_down = t;
						} else if (e.value == 0) {
							if (power_down && !combo) {
								screen_on = !screen_on;
								sysfs_write_int(BL, screen_on ? saved_bl : 0);
								if (!screen_on && scene == ADULT)
									kid_home();
								dirty = 1;
							}
							power_down = 0;
							combo &= back_down != 0;
						}
					} else if (!screen_on && e.type == EV_KEY && e.value == 1 &&
						   (e.code == KEY_BACK || e.code == BTN_TOUCH)) {
						/* A dark screen wakes on a tap or the second button; that press does nothing else. */
						screen_on = 1;
						sysfs_write_int(BL, saved_bl);
						dirty = 1;
						if (e.code == BTN_TOUCH)
							touch_down = 1;
					} else if (e.type == EV_KEY && e.code == KEY_BACK) {
						if (e.value == 1) {
							back_down = t;
						} else if (e.value == 0) {
							if (back_down && !combo && screen_on) {
								if (scene == ADULT)
									adult_back();
								else if (scene != HOME)
									enter(HOME);
								dirty = 1;
							}
							back_down = 0;
							combo &= power_down != 0;
						}
					} else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
						touch_down = e.value;
						ev_pending = e.value ? EV_DOWN : EV_UP;
					} else if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_X || e.code == ABS_X)) {
						touch_x = e.value;
						if (touch_down && ev_pending == EV_NONE)
							ev_pending = EV_MOVE;
					} else if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_Y || e.code == ABS_Y)) {
						touch_y = e.value;
						if (touch_down && ev_pending == EV_NONE)
							ev_pending = EV_MOVE;
					} else if (e.type == EV_SYN && ev_pending != EV_NONE) {
						if (screen_on)
							handle_touch(ev_pending, touch_x, touch_y, t);
						ev_pending = EV_NONE;
						dirty = 1;
					}
				}
			}
		}

		/* Screen off after a minute without input (the battery lasts much longer). */
		if (screen_on && t - last_input > SCREEN_TIMEOUT_MS) {
			screen_on = 0;
			sysfs_write_int(BL, 0);
			if (scene == ADULT)
				kid_home();
		}

		/* Both buttons held 3 s: grown-up mode (the sum comes first). */
		if (back_down && power_down && !combo &&
		    t - (back_down > power_down ? back_down : power_down) > 3000) {
			combo = 1;
			if (scene != ADULT)
				enter(ADULT);
			dirty = 1;
		}

		/* Parent escape: hold the second button alone for 3 s -> reboot (back to Android). */
		if (back_down && !power_down && !combo && t - back_down > 3000)
			bye();

		if (t - last_batt > 30000) {
			battery = battery_pct();
			last_batt = t;
			dirty |= scene == HOME;
		}

		if (scene == POP)
			pop_tick(t);
		if (scene == MATCH)
			match_tick(t);
		if (scene == PARADE)
			parade_tick(t);
		if (scene == ADULT)
			dirty |= adult_tick(t);

		if (!screen_on || (!dirty && !animating) || t - last_frame < 30)
			continue;
		switch (scene) {
		case HOME: draw_home(); break;
		case POP: draw_pop(t); break;
		case MATCH: draw_match(); break;
		case PAINT: draw_paint(); break;
		case PARADE: draw_parade(t); break;
		case ADULT: adult_draw(); break;
		}
		present();
		last_frame = t;
		dirty = 0;
	}
}
