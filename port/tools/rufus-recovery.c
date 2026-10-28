/*
 * The screen shown when the UI keeps crashing (rufus-ui-run starts it after 3 quick crashes).
 *   rufus-recovery [REASON]
 * Prints on the kernel console (text mode again: pupui leaves tty0 in KD_GRAPHICS), so it
 * needs none of the UI's code. Lower button: exit, and rufus-ui-run starts the UI again.
 * Upper button held 3 s: back to Android (rufus-android).
 * The console is 40 x 15 characters (TER16x32 on the 640x480 landscape screen).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BL "/sys/class/backlight/backlight/brightness"
#define BL_MAX "/sys/class/backlight/backlight/max_brightness"

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void backlight_on(void)
{
	char s[16] = "16";
	FILE *f = fopen(BL_MAX, "r");
	int max = 16;

	if (f) {
		if (fscanf(f, "%d", &max) != 1)
			max = 16;
		fclose(f);
	}
	f = fopen(BL, "w");
	if (f) {
		snprintf(s, sizeof(s), "%d", max * 3 / 4);
		fputs(s, f);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	struct pollfd fds[8];
	int n = 0;
	int tty = open("/dev/tty0", O_RDWR);
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	if (tty >= 0) {
		ioctl(tty, KDSETMODE, KD_TEXT);
		dprintf(tty, "\033[2J\033[H\033[?25l"
			"\n  RufusOS stopped working.\n\n"
			"  %.36s\n\n"
			"  Lower button:\n"
			"      start it again\n\n"
			"  Upper button, hold 3 s:\n"
			"      back to Android\n\n"
			"  Log: /var/log/rufus-ui.log\n",
			argc > 1 ? argv[1] : "It crashed several times.");
	}
	backlight_on();

	while (d && (e = readdir(d)) && n < 8) {
		char p[300];
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		fds[n].fd = open(p, O_RDONLY | O_NONBLOCK);
		fds[n].events = POLLIN;
		if (fds[n].fd >= 0)
			n++;
	}
	if (d)
		closedir(d);

	long back_down = 0;
	for (;;) {
		poll(fds, n, back_down ? 100 : -1);
		for (int i = 0; i < n; i++) {
			struct input_event ev;
			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY)
					continue;
				if (ev.code == KEY_POWER && ev.value == 0)
					goto restart;
				if (ev.code == KEY_BACK)
					back_down = ev.value ? now_ms() : 0;
			}
		}
		if (back_down && now_ms() - back_down > 3000) {
			if (tty >= 0)
				dprintf(tty, "\n  Rebooting to Android ...\n");
			execl("/usr/local/bin/rufus-android", "rufus-android", (char *)NULL);
			back_down = 0;	/* not installed (initramfs): stay here */
		}
	}
restart:
	if (tty >= 0)
		dprintf(tty, "\033[2J\033[H");
	return 0;
}
