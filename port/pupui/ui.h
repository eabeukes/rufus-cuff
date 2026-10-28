/* pupui: what pupui.c shares with grownup.c and theme.c. */
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
void kid_home(void);		/* leave grown-up mode */
void bye(void);			/* clean reboot, back to Android */

/* grownup.c: the parents' side, behind a sum. */
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
