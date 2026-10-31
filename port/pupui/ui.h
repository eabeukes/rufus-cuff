/* pupui: what pupui.c (main loop, Kids mode) shares with rufus.c, grownup.c and theme.c. */
#include <stdint.h>

#include "assets.h"

#define W 640
#define H 480

extern uint32_t cv[H][W];	/* landscape canvas, shown by present() */

long now_ms(void);
int rnd(int n);

void fill_rect(int x, int y, int w, int h, uint32_t c);
void fill_circle(int cx, int cy, int r, uint32_t c);
void fill_rrect(int x, int y, int w, int h, int r, uint32_t c);
void blit(const struct image *im, int x, int y);
int text_width(const struct font *f, const char *s);
void text(const struct font *f, int x, int y, uint32_t c, const char *s);
void text_c(const struct font *f, int cx, int y, uint32_t c, const char *s);
void present(void);

int sysfs_read_int(const char *path);
void sysfs_write_int(const char *path, int v);

#define BL "/sys/class/backlight/backlight/brightness"
extern int saved_bl;		/* brightness to restore when the screen comes back on */

enum { EV_NONE, EV_DOWN, EV_MOVE, EV_UP };

/* pupui.c */
extern int plugged;		/* charger connected (battery_pct() updates it) */
int battery_pct(void);
void kid_home(void);		/* back to the pups (gate cancelled) */
void kids_enter(void);		/* RufusOS -> Kids mode */
void kids_unlock(void);		/* the gate was passed: Kids mode -> RufusOS */
void settings_done(void);	/* Settings closed: back to RufusOS */
void settings_open(int what);	/* RufusOS -> Settings (SETTINGS_*) */
void bye(void);			/* clean reboot, back to Android */

/* rufus.c: RufusOS, the main UI. */
#define RUFUS_REDRAW 1
#define RUFUS_WAKE 2
#define RUFUS_SHOW 4		/* bring RufusOS forward (the alarm rings) */
void rufus_enter(void);
void rufus_draw(long t);
void rufus_touch(int ev, int x, int y, long t);
void rufus_back(void);
int rufus_tick(long t, int kids);	/* RUFUS_REDRAW | RUFUS_WAKE | RUFUS_SHOW */
void rufus_alarm_input(void);	/* a button or touch in Kids mode: stop a ringing alarm */
int rufus_animating(void);
long rufus_sleep_ms(long t, int kids);	/* how long the watch may sleep: 0 none, -1 no limit */
void rufus_bench(void);		/* pupui --bench: time each view's drawing */
double bench_ms(void (*fn)(void), int n);

/* grownup.c: Settings (Wi-Fi, terminal, status, desktop), and the sum gate out of Kids mode. */
enum { SETTINGS_MENU, SETTINGS_TERMINAL, SETTINGS_DESKTOP };
#define START_KIDS "/var/lib/rufus/start-in-kids"	/* boot into Kids mode when present */
void adult_open(int what);	/* Settings without the gate */
int hit(int x, int y, int bx, int by, int bw, int bh);
void text_mid(const struct font *f, int cx, int y, uint32_t c, const char *s);
const char *fit(const struct font *f, const char *s, int w);
void button(int x, int y, int w, int h, const char *label, uint32_t bg);
void adult_enter(void);		/* the sum gate first */
void adult_leave(void);
void adult_draw(void);
void adult_touch(int ev, int x, int y, long t);
void adult_back(void);		/* second button: up one level */
int adult_tick(long t);		/* nonzero: redraw */
int adult_fd(void);		/* terminal output to poll, or -1 */
void adult_io(void);		/* adult_fd() is readable */

/* theme.c: replacement pups, badges, names and home picture, if installed. */
void theme_load(void);
