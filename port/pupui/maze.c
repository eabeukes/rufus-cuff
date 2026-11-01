/*
 * Pup Maze (Kids mode), Leilah's game. Level 1: the pups are lost around a hedge maze, one in
 * each coloured area; tap a spot and Rory walks there, and each pup he reaches follows him.
 * Level 2: on the way back to the Lookout Rory gets lost in Adventure City; she picks a pup
 * and guides it through the streets to him, the others tagging along. Every game has new
 * mazes, a size bigger after each finished game.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define BAR 80			/* title bar; the maze fills the rest */
#define MAXC 10
#define MAXR 6
#define SPR_MAX 84		/* largest sprite (the smallest maze's cells) */
#define TRAIL 128		/* walker positions kept for the followers */
#define GAP 12			/* trail points between followers (each >= 3 px apart) */
#define SPEED 260		/* walking speed, px/s */
#define CHEER_MS 2500		/* "You found ...!" before play goes on */

/* Maze sizes in cells, smallest first: level 1 uses sizes[level], level 2 the next one. */
static const struct { int c, r; } sizes[] = {
	{6, 4}, {7, 4}, {7, 5}, {8, 5}, {8, 6}, {9, 6}, {10, 6},
};
#define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

enum { L1, FOUND, PICK, L2, RESCUED, WIN };

/* Art scaled down from the generated images (or a theme's) to fit the maze cells. */
struct sprite { struct image im; uint32_t px[SPR_MAX * SPR_MAX]; };

static const int dc[4] = {1, -1, 0, 0}, dr[4] = {0, 0, 1, -1};

static struct {
	int phase, level, cols, rows, cw, ch, ox, oy;
	uint8_t open[MAXR][MAXC];	/* bit 0: open to the right, bit 1: open below */
	int pup_c[6], pup_r[6], found[6];
	int order[6], nfollow;		/* the followers, nearest first */
	int hero;			/* level 2: the pup she picked */
	int goal_c, goal_r;		/* level 2: where Rory is */
	double x, y;			/* the walker, canvas px */
	int wc, wr;			/* the cell it is at or walking to */
	int path_c[MAXC * MAXR + 1], path_r[MAXC * MAXR + 1], path_n, path_i;
	double hx[TRAIL], hy[TRAIL];
	int hhead, hcount;
	double stamp;			/* px walked since the last footprint */
	int show;			/* FOUND: which pup */
	long last, since;
} mz;

static uint32_t bg[H][W];		/* this level's maze, drawn once; footprints added */
static struct sprite spr_pup[6], spr_follow[6], spr_rory, spr_badge[6], tile_rory, tile_pup;

static void scale(struct sprite *s, const struct image *src, int size)
{
	s->im.w = s->im.h = size;
	s->im.px = s->px;
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			int x0 = x * src->w / size, x1 = (x + 1) * src->w / size;
			int y0 = y * src->h / size, y1 = (y + 1) * src->h / size;
			unsigned a = 0, r = 0, g = 0, b = 0, n = 0;

			for (int j = y0; j < (y1 > y0 ? y1 : y0 + 1); j++)
				for (int i = x0; i < (x1 > x0 ? x1 : x0 + 1); i++) {
					uint32_t p = src->px[j * src->w + i];
					unsigned pa = p >> 24;

					a += pa;
					r += (p >> 16 & 0xff) * pa;
					g += (p >> 8 & 0xff) * pa;
					b += (p & 0xff) * pa;
					n++;
				}
			s->px[y * size + x] = a ? (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | b / a : 0;
		}
}

static void put(const struct sprite *s, double cx, double cy)
{
	blit(&s->im, (int)cx - s->im.w / 2, (int)cy - s->im.h / 2);
}

static int cell_x(int c) { return mz.ox + c * mz.cw + mz.cw / 2; }
static int cell_y(int r) { return mz.oy + r * mz.ch + mz.ch / 2; }

static int is_open(int c, int r, int d)
{
	int nc = c + dc[d], nr = r + dr[d];

	if (nc < 0 || nc >= mz.cols || nr < 0 || nr >= mz.rows)
		return 0;
	switch (d) {
	case 0: return mz.open[r][c] & 1;
	case 1: return mz.open[r][nc] & 1;
	case 2: return mz.open[r][c] & 2;
	default: return mz.open[nr][c] & 2;
	}
}

/* A new maze: a random depth-first tree, plus loops so a wrong turn is never far from right. */
static void carve(int size)
{
	int sc[MAXC * MAXR], sr[MAXC * MAXR], n = 1;
	uint8_t seen[MAXR][MAXC] = {{0}};

	mz.cols = sizes[size].c;
	mz.rows = sizes[size].r;
	mz.cw = W / mz.cols;
	mz.ch = (H - BAR) / mz.rows;
	mz.ox = (W - mz.cw * mz.cols) / 2;
	mz.oy = BAR + (H - BAR - mz.ch * mz.rows) / 2;
	memset(mz.open, 0, sizeof(mz.open));
	sc[0] = sr[0] = 0;
	seen[0][0] = 1;
	while (n) {
		int c = sc[n - 1], r = sr[n - 1], dirs[4], k = 0;

		for (int d = 0; d < 4; d++) {
			int nc = c + dc[d], nr = r + dr[d];
			if (nc >= 0 && nc < mz.cols && nr >= 0 && nr < mz.rows && !seen[nr][nc])
				dirs[k++] = d;
		}
		if (!k) {
			n--;
			continue;
		}
		int d = dirs[rnd(k)], nc = c + dc[d], nr = r + dr[d];
		if (d == 0)
			mz.open[r][c] |= 1;
		else if (d == 1)
			mz.open[r][nc] |= 1;
		else if (d == 2)
			mz.open[r][c] |= 2;
		else
			mz.open[nr][c] |= 2;
		seen[nr][nc] = 1;
		sc[n] = nc;
		sr[n] = nr;
		n++;
	}
	for (int i = 0; i < mz.cols * mz.rows / 5; i++) {
		if (rnd(2))
			mz.open[rnd(mz.rows)][rnd(mz.cols - 1)] |= 1;
		else
			mz.open[rnd(mz.rows - 1)][rnd(mz.cols)] |= 2;
	}

	int s = mz.cw < mz.ch ? mz.cw : mz.ch;
	s = s * 4 / 5 > SPR_MAX ? SPR_MAX : s * 4 / 5;
	for (int i = 0; i < 6; i++) {
		scale(&spr_pup[i], pups[i], s);
		scale(&spr_follow[i], pups[i], s * 3 / 4);
	}
	scale(&spr_rory, &rory, s);
}

/* Steps from (c, r) to every cell; prev leads back towards (c, r). */
static void bfs(int c, int r, int dist[MAXR][MAXC], int prev[MAXR][MAXC])
{
	int qc[MAXC * MAXR], qr[MAXC * MAXR], head = 0, tail = 0;

	for (int j = 0; j < mz.rows; j++)
		for (int i = 0; i < mz.cols; i++)
			dist[j][i] = -1;
	dist[r][c] = 0;
	qc[tail] = c;
	qr[tail++] = r;
	while (head < tail) {
		int pc = qc[head], pr = qr[head++];
		for (int d = 0; d < 4; d++) {
			int nc = pc + dc[d], nr = pr + dr[d];
			if (!is_open(pc, pr, d) || dist[nr][nc] >= 0)
				continue;
			dist[nr][nc] = dist[pr][pc] + 1;
			prev[nr][nc] = d;
			qc[tail] = nc;
			qr[tail++] = nr;
		}
	}
}

/* Walk to cell (c, r): first on to the cell already being walked to, then the shortest way. */
static void walk_to(int c, int r)
{
	int dist[MAXR][MAXC], prev[MAXR][MAXC], n;
	int sc = mz.path_n ? mz.path_c[mz.path_i] : mz.wc, sr = mz.path_n ? mz.path_r[mz.path_i] : mz.wr;

	bfs(sc, sr, dist, prev);
	n = dist[r][c] + 1;
	for (int i = n - 1; i >= 0; i--) {
		mz.path_c[i] = c;
		mz.path_r[i] = r;
		if (i) {
			int d = prev[r][c];
			c -= dc[d];
			r -= dr[d];
		}
	}
	mz.path_n = n;
	mz.path_i = 0;
}

static void trail_reset(void)
{
	mz.hhead = mz.hcount = 0;
	mz.hx[0] = mz.x;
	mz.hy[0] = mz.y;
	mz.hcount = 1;
}

/* Points every 3 px along the way walked, however far a frame moved it. */
static void trail_push(void)
{
	for (;;) {
		double lx = mz.hx[mz.hhead], ly = mz.hy[mz.hhead];
		double dx = mz.x - lx, dy = mz.y - ly, d = sqrt(dx * dx + dy * dy);

		if (d < 3)
			return;
		mz.hhead = (mz.hhead + 1) % TRAIL;
		mz.hx[mz.hhead] = lx + dx * 3 / d;
		mz.hy[mz.hhead] = ly + dy * 3 / d;
		if (mz.hcount < TRAIL)
			mz.hcount++;
	}
}

static void trail_at(int back, double *x, double *y)
{
	if (back >= mz.hcount)
		back = mz.hcount - 1;
	int i = (mz.hhead - back + TRAIL) % TRAIL;
	*x = mz.hx[i];
	*y = mz.hy[i];
}

/* ---------------------------------------------------------------- drawing the mazes */

static uint32_t mix(uint32_t a, uint32_t b) { return 0xff000000 | (((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1)); }

static void bg_dot(int cx, int cy, int rad, uint32_t c)
{
	for (int y = cy - rad; y <= cy + rad; y++)
		for (int x = cx - rad; x <= cx + rad; x++)
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad && x >= 0 && x < W && y >= BAR && y < H)
				bg[y][x] = mix(bg[y][x], c);
}

/* A wall from (x0, y0) to (x1, y1), t thick: a hedge (park) or a building (city). */
static void hedge(int x0, int y0, int x1, int y1, int t)
{
	fill_rrect(x0 - t / 2 - 2, y0 - t / 2 - 2, x1 - x0 + t + 4, y1 - y0 + t + 4, t / 2 + 2, 0xff2e6e2c);
	fill_rrect(x0 - t / 2, y0 - t / 2, x1 - x0 + t, y1 - y0 + t, t / 2, 0xff4aa042);
}

static void building(int x0, int y0, int x1, int y1, int t)
{
	static const uint32_t cols[5] = {0xe8785a, 0xf0c450, 0x78b4e6, 0xaa82d2, 0x6ec496};
	uint32_t c = cols[rnd(5)];
	uint32_t edge = 0xff000000 | ((c >> 16 & 0xff) * 7 / 10) << 16 | ((c >> 8 & 0xff) * 7 / 10) << 8 | (c & 0xff) * 7 / 10;
	int bx = x0 - t / 2, by = y0 - t / 2, bw = x1 - x0 + t, bh = y1 - y0 + t;

	fill_rect(bx, by, bw, bh, edge);
	fill_rect(bx + 3, by + 3, bw - 6, bh - 6, 0xff000000 | c);
	for (int y = by + 7; y + 7 < by + bh - 4; y += 14)
		for (int x = bx + 7; x + 7 < bx + bw - 4; x += 14)
			fill_rect(x, y, 7, 7, 0xfffffac8);
}

static void draw_walls(int t, void (*wall)(int, int, int, int, int))
{
	int x1 = mz.ox + mz.cols * mz.cw, y1 = mz.oy + mz.rows * mz.ch;

	for (int r = 0; r < mz.rows; r++)
		for (int c = 0; c < mz.cols; c++) {
			int x = mz.ox + c * mz.cw, y = mz.oy + r * mz.ch;
			if (c + 1 < mz.cols && !(mz.open[r][c] & 1))
				wall(x + mz.cw, y, x + mz.cw, y + mz.ch, t);
			if (r + 1 < mz.rows && !(mz.open[r][c] & 2))
				wall(x, y + mz.ch, x + mz.cw, y + mz.ch, t);
		}
	wall(mz.ox, mz.oy, x1, mz.oy, t);
	wall(mz.ox, y1, x1, y1, t);
	wall(mz.ox, mz.oy, mz.ox, y1, t);
	wall(x1, mz.oy, x1, y1, t);
}

static int zone(int c, int r) { return c * 3 / mz.cols + 3 * (r * 2 / mz.rows); }

/* The park: sand paths, each pup's area tinted with its colour, hedges. */
static void draw_park(void)
{
	for (int r = 0; r < mz.rows; r++)
		for (int c = 0; c < mz.cols; c++) {
			uint32_t t = pup_team[zone(c, r)];
			uint32_t tint = 0xff000000 | (236 * 4 + (t >> 16 & 0xff)) / 5 << 16 |
					(226 * 4 + (t >> 8 & 0xff)) / 5 << 8 | (190 * 4 + (t & 0xff)) / 5;
			fill_rect(mz.ox + c * mz.cw, mz.oy + r * mz.ch, mz.cw, mz.ch, tint);
		}
	draw_walls((mz.cw < mz.ch ? mz.cw : mz.ch) / 7, hedge);
}

/* Adventure City: roads with lane markings, lined with buildings. */
static void draw_city(void)
{
	fill_rect(0, BAR, W, H - BAR, 0xff60646e);
	for (int r = 0; r < mz.rows; r++)
		for (int c = 0; c < mz.cols; c++) {
			int x = cell_x(c), y = cell_y(r);
			for (int k = 0; k < 3; k++) {
				if (mz.open[r][c] & 1)
					fill_rect(x + 12 + k * mz.cw / 3, y - 2, 10, 4, 0xfffad63c);
				if (mz.open[r][c] & 2)
					fill_rect(x - 2, y + 12 + k * mz.ch / 3, 4, 10, 0xfffad63c);
			}
		}
	draw_walls((mz.cw < mz.ch ? mz.cw : mz.ch) / 4, building);
}

static void save_bg(void)
{
	fill_rect(0, 0, W, BAR, 0xff000000);
	memcpy(bg, cv, sizeof(bg));
}

/* ---------------------------------------------------------------- the game */

static void level1(void)
{
	int size = mz.level < NSIZES - 2 ? mz.level : NSIZES - 2;

	carve(size);
	mz.phase = L1;
	mz.wc = mz.cols / 2;
	mz.wr = mz.rows / 2;
	for (int z = 0; z < 6; z++) {	/* a pup somewhere in each area, not on Rory */
		int n = 0, cc[MAXC * MAXR], rr[MAXC * MAXR];
		for (int r = 0; r < mz.rows; r++)
			for (int c = 0; c < mz.cols; c++)
				if (zone(c, r) == z && (c != mz.wc || r != mz.wr)) {
					cc[n] = c;
					rr[n++] = r;
				}
		int k = rnd(n);
		mz.pup_c[z] = cc[k];
		mz.pup_r[z] = rr[k];
		mz.found[z] = 0;
	}
	mz.nfollow = 0;
	mz.x = cell_x(mz.wc);
	mz.y = cell_y(mz.wr);
	mz.path_n = 0;
	trail_reset();
	draw_park();
	save_bg();
}

static void level2(int hero)
{
	int size = mz.level + 1 < NSIZES ? mz.level + 1 : NSIZES - 1;
	int dist[MAXR][MAXC], prev[MAXR][MAXC], best = -1;

	carve(size);
	mz.phase = L2;
	mz.hero = hero;
	mz.nfollow = 0;
	for (int i = 0; i < 6; i++)
		if (i != hero)
			mz.order[mz.nfollow++] = i;
	mz.wc = 0;
	mz.wr = mz.rows - 1;
	bfs(mz.wc, mz.wr, dist, prev);	/* Rory is lost as far away as can be */
	for (int r = 0; r < mz.rows; r++)
		for (int c = 0; c < mz.cols; c++)
			if (dist[r][c] > best) {
				best = dist[r][c];
				mz.goal_c = c;
				mz.goal_r = r;
			}
	mz.x = cell_x(mz.wc);
	mz.y = cell_y(mz.wr);
	mz.path_n = 0;
	trail_reset();
	draw_city();
	save_bg();
}

void maze_start(void)
{
	for (int i = 0; i < 6; i++)
		scale(&spr_badge[i], badges[i], 32);
	level1();
	mz.last = now_ms();
}

static void arrive(long t)
{
	int c = mz.path_c[mz.path_i], r = mz.path_r[mz.path_i];

	mz.wc = c;
	mz.wr = r;
	if (++mz.path_i >= mz.path_n)
		mz.path_n = 0;
	if (mz.phase == L1) {
		for (int i = 0; i < 6; i++)
			if (!mz.found[i] && mz.pup_c[i] == c && mz.pup_r[i] == r) {
				mz.found[i] = 1;
				mz.order[mz.nfollow++] = i;
				mz.show = i;
				mz.phase = FOUND;
				mz.since = t;
				mz.path_n = 0;
			}
	} else if (c == mz.goal_c && r == mz.goal_r) {
		mz.phase = RESCUED;
		mz.since = t;
		mz.path_n = 0;
	}
}

void maze_tick(long t)
{
	double step = SPEED * (t - mz.last) / 1000.0;

	mz.last = t;
	if (step > SPEED / 10.0)
		step = SPEED / 10.0;
	if ((mz.phase == FOUND || mz.phase == RESCUED) && t - mz.since > CHEER_MS) {
		if (mz.phase == RESCUED) {
			mz.phase = WIN;
			mz.level++;
		} else {
			mz.phase = mz.nfollow == 6 ? PICK : L1;
		}
		mz.since = t;
	}
	while ((mz.phase == L1 || mz.phase == L2) && mz.path_n && step > 0) {
		double tx = cell_x(mz.path_c[mz.path_i]), ty = cell_y(mz.path_r[mz.path_i]);
		double dx = tx - mz.x, dy = ty - mz.y, d = sqrt(dx * dx + dy * dy);

		if (d <= step) {
			mz.stamp += d;
			mz.x = tx;
			mz.y = ty;
			step -= d;
			arrive(t);
		} else {
			mz.stamp += step;
			mz.x += dx * step / d;
			mz.y += dy * step / d;
			step = 0;
		}
		trail_push();
		if (mz.stamp > 18) {	/* footprints */
			mz.stamp = 0;
			bg_dot((int)mz.x, (int)mz.y + 6, 4, mz.phase == L2 ? 0xffe6e6e6 : 0xff967850);
		}
	}
}

int maze_animating(void)
{
	return ((mz.phase == L1 || mz.phase == L2) && mz.path_n) || mz.phase == FOUND ||
	       mz.phase == RESCUED || mz.phase == WIN;
}

static void paw(int x, int y, uint32_t c)
{
	static const int toe[4][2] = {{-12, -8}, {-5, -15}, {5, -15}, {12, -8}};

	fill_circle(x, y + 5, 12, 0xffffffff);
	fill_circle(x, y + 5, 9, c);
	for (int i = 0; i < 4; i++) {
		fill_circle(x + toe[i][0], y + toe[i][1], 6, 0xffffffff);
		fill_circle(x + toe[i][0], y + toe[i][1], 4, c);
	}
}

static void bubble(int cx, int y, const char *s, const struct font *f)
{
	int w = text_width(f, s);

	fill_rrect(cx - w / 2 - 16, y, w + 32, f->height + 12, (f->height + 12) / 2, 0xff303030);
	fill_rrect(cx - w / 2 - 13, y + 3, w + 26, f->height + 6, (f->height + 6) / 2, 0xffffffff);
	text(f, cx - w / 2, y + 6, 0x202020, s);
}

static void dim(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			cv[y][x] = 0xff000000 | (cv[y][x] >> 1 & 0x7f7f7f);
}

static void cheer(long t, const struct image *a, const struct image *b, const char *msg)
{
	dim();
	for (int k = 0; k < 14; k++) {
		double ang = k * 2 * M_PI / 14 + t / 900.0;
		star(W / 2 + (int)(200 * cos(ang)), 220 + (int)(130 * sin(ang)), 16, 0xffffd600);
	}
	if (b) {
		blit(a, W / 2 - 160, 120);
		blit(b, W / 2 + 10, 120);
	} else {
		blit(a, W / 2 - a->w / 2, 120);
	}
	bubble(W / 2, 320, msg, &font_m);
}

static void banner(int cx, int y, const char *s, const struct font *f, uint32_t c)
{
	int w = text_width(f, s);

	fill_rrect(cx - w / 2 - 22, y - 8, w + 44, f->height + 12, (f->height + 12) / 2, c);
	text_c(f, cx, y, 0xffffff, s);
}

static void draw_play(long t)
{
	int moving = mz.path_n > 0, bob = moving ? (int)(4 * fabs(sin(t / 90.0))) : 0;

	memcpy(cv, bg, sizeof(cv));
	if (mz.phase == L1 || mz.phase == FOUND) {
		fill_rect(0, 0, W, BAR, 0xff2654c8);
		text_c(&font_m, 252, 18, 0xffffff, "Find the pups!");
		for (int i = 0; i < 6; i++) {
			int x = 432 + i * 34;
			if (mz.found[i]) {
				blit(&spr_badge[i].im, x, 24);
			} else {
				fill_circle(x + 16, 40, 16, 0xffffffff);
				fill_circle(x + 16, 40, 13, 0xff173380);
				text_c(&font_s, x + 16, 25, 0xffffff, "?");
			}
		}
	} else {
		fill_rect(0, 0, W, BAR, 0xffe22626);
		text_c(&font_m, 220, 18, 0xffffff, "Find Rory!");
		text_c(&font_s, 500, 28, 0xffffff, "Adventure City");
	}
	home_button();

	if (moving) {	/* where the walker is going: dots to a paw mark */
		uint32_t c = mz.phase == L2 ? 0xffffd600 : 0xffe22626;
		double px = mz.x, py = mz.y;
		for (int i = mz.path_i; i < mz.path_n; i++) {
			double tx = cell_x(mz.path_c[i]), ty = cell_y(mz.path_r[i]);
			int n = (int)(sqrt((tx - px) * (tx - px) + (ty - py) * (ty - py)) / 16);
			for (int k = 1; k <= n; k++)
				fill_circle((int)(px + (tx - px) * k / n), (int)(py + (ty - py) * k / n), 4, c);
			px = tx;
			py = ty;
		}
		paw(cell_x(mz.path_c[mz.path_n - 1]), cell_y(mz.path_r[mz.path_n - 1]), c);
	}

	if (mz.phase == L1 || mz.phase == FOUND) {
		for (int i = 0; i < 6; i++)
			if (!mz.found[i])
				put(&spr_pup[i], cell_x(mz.pup_c[i]), cell_y(mz.pup_r[i]));
	} else {
		int gx = cell_x(mz.goal_c), gy = cell_y(mz.goal_r);
		put(&spr_rory, gx, gy);
		bubble(gx < W / 2 ? gx + 60 : gx - 60, gy - mz.ch / 2 - 6 < BAR ? gy + 20 : gy - mz.ch / 2 - 6,
		       "Help!", &font_s);
	}
	for (int k = mz.nfollow - 1; k >= 0; k--) {
		double fx, fy;
		trail_at((k + 1) * GAP, &fx, &fy);
		put(&spr_follow[mz.order[k]], fx, fy + 8);
	}
	put(mz.phase == L2 || mz.phase == RESCUED ? &spr_pup[mz.hero] : &spr_rory, mz.x, mz.y - bob);
}

static void pick_rect(int i, int *x, int *y) { *x = 30 + (i % 3) * 200; *y = 140 + (i / 3) * 165; }

static void draw_pick(void)
{
	blit(home_img, 0, 0);
	dim();
	banner(W / 2, 22, "Oh no! Rory is lost!", &font_m, 0xffe22626);
	text_c(&font_s, W / 2, 88, 0xffffff, "Who will find him?");
	for (int i = 0; i < 6; i++) {
		int x, y;
		pick_rect(i, &x, &y);
		fill_rrect(x, y, 180, 150, 20, 0xff000000 | pup_team[i]);
		fill_rrect(x + 6, y + 6, 168, 106, 14, 0xffffffff);
		blit(pups_small[i], x + 42, y + 10);
		text_c(&font_s, x + 90, y + 116, 0xffffff, pup_names[i]);
	}
}

#define AGAIN_X 452
#define AGAIN_Y 396
#define AGAIN_W 172

static void draw_win(long t)
{
	blit(home_img, 0, 0);
	for (int i = 0; i < 40; i++) {	/* confetti */
		int x = (i * 97 + (int)(t / 40) % 23 * (i % 3)) % W;
		int y = 70 + (int)((t / (8 + i % 5) + i * 53) % 330);
		fill_rect(x, y, 8, 12, 0xff000000 | pup_team[i % 6]);
	}
	banner(W / 2, 22, "Home to the Lookout!", &font_m, 0xff2654c8);
	blit(&rory, 20, 300);
	for (int i = 0; i < 6; i++)
		blit(pups_small[i], 170 + (i % 3) * 90, 250 + (i / 3) * 90 - (int)(8 * fabs(sin(t / 200.0 + i))));
	fill_rrect(AGAIN_X, AGAIN_Y, AGAIN_W, 60, 22, 0xff965ad2);
	text_c(&font_s, AGAIN_X + AGAIN_W / 2, AGAIN_Y + 14, 0xffffff, "Play again");
}

void maze_draw(long t)
{
	switch (mz.phase) {
	case PICK:
		draw_pick();
		break;
	case WIN:
		draw_win(t);
		break;
	default:
		draw_play(t);
		if (mz.phase == FOUND) {
			char s[48];
			snprintf(s, sizeof(s), "You found %s!", pup_names[mz.show]);
			cheer(t, pups[mz.show], NULL, s);
		} else if (mz.phase == RESCUED) {
			cheer(t, &rory, pups[mz.hero], "You found Rory!");
		}
	}
}

void maze_touch(int x, int y, long t)
{
	switch (mz.phase) {
	case L1:
	case L2:
		if (y >= mz.oy && y < mz.oy + mz.rows * mz.ch && x >= mz.ox && x < mz.ox + mz.cols * mz.cw)
			walk_to((x - mz.ox) / mz.cw, (y - mz.oy) / mz.ch);
		break;
	case FOUND:
	case RESCUED:
		mz.since = t - CHEER_MS - 1;	/* a tap skips the cheer */
		break;
	case PICK:
		for (int i = 0; i < 6; i++) {
			int px, py;
			pick_rect(i, &px, &py);
			if (x >= px && x < px + 180 && y >= py && y < py + 150) {
				level2(i);
				mz.last = t;
			}
		}
		break;
	case WIN:
		if (x >= AGAIN_X && x < AGAIN_X + AGAIN_W && y >= AGAIN_Y && y < AGAIN_Y + 60) {
			level1();
			mz.last = t;
		}
		break;
	}
}

/* The home tile's picture, in its 144x118 white panel at (x, y): a little maze, Rory, a pup. */
void maze_tile(int x, int y)
{
	static const short w[][4] = {	/* x, y, w, h */
		{8, 8, 128, 8}, {8, 102, 128, 8}, {8, 8, 8, 102}, {128, 8, 8, 102},
		{38, 8, 8, 62}, {70, 40, 8, 70}, {102, 8, 8, 68},
	};

	if (!tile_rory.im.w) {
		scale(&tile_rory, &rory, 46);
		scale(&tile_pup, pups[1], 40);
	}
	for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++)
		fill_rrect(x + w[i][0], y + w[i][1], w[i][2], w[i][3], 4, 0xff4aa042);
	blit(&tile_rory.im, x + 14, y + 58);
	blit(&tile_pup.im, x + 84, y + 64);
}
