/*
 * Inject touches into the touchscreen's evdev node, for testing UIs without a finger.
 *   tap X Y [X2 Y2]   tap at (X, Y), or drag from (X, Y) to (X2, Y2)
 *   tap key CODE 0|1  release or press a key (116 power, 158 back) on its own device
 * Coordinates are the wearer's 640x480 (as reported after the DT's swap/invert).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>

static int fd;

static void ev(int type, int code, int value)
{
	struct input_event e = { .type = type, .code = code, .value = value };

	if (write(fd, &e, sizeof(e)) != sizeof(e))
		perror("write");
}

static void at(int x, int y)
{
	ev(EV_ABS, ABS_MT_POSITION_X, x);
	ev(EV_ABS, ABS_MT_POSITION_Y, y);
	ev(EV_ABS, ABS_X, x);
	ev(EV_ABS, ABS_Y, y);
}

/* The first input device whose name contains want, or (want NULL) that has key code key. */
static int find(const char *want, int key)
{
	char path[300], name[64];
	unsigned char bits[KEY_MAX / 8 + 1];
	struct dirent *d;
	DIR *dir = opendir("/dev/input");

	while (dir && (d = readdir(dir))) {
		if (strncmp(d->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", d->d_name);
		int f = open(path, O_RDWR);
		if (f < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (want ? ioctl(f, EVIOCGNAME(sizeof(name)), name) > 0 && strstr(name, want)
			 : ioctl(f, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) > 0 && (bits[key / 8] >> (key % 8) & 1))
			return f;
		close(f);
	}
	return -1;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "key")) {
		int code = atoi(argv[2]);
		if (code <= 0 || code > KEY_MAX || (fd = find(NULL, code)) < 0) {
			fprintf(stderr, "no device with key %d\n", code);
			return 1;
		}
		ev(EV_KEY, code, atoi(argv[3]));
		ev(EV_SYN, SYN_REPORT, 0);
		return 0;
	}
	if (argc != 3 && argc != 5) {
		fprintf(stderr, "usage: tap X Y [X2 Y2] | tap key CODE 0|1\n");
		return 2;
	}
	fd = find("ft5x06", 0);
	if (fd < 0) {
		fprintf(stderr, "no touchscreen\n");
		return 1;
	}

	int x = atoi(argv[1]), y = atoi(argv[2]);
	ev(EV_ABS, ABS_MT_TRACKING_ID, 1);
	at(x, y);
	ev(EV_KEY, BTN_TOUCH, 1);
	ev(EV_SYN, SYN_REPORT, 0);
	if (argc == 5) {
		int x2 = atoi(argv[3]), y2 = atoi(argv[4]);
		for (int s = 1; s <= 20; s++) {
			usleep(15000);
			at(x + (x2 - x) * s / 20, y + (y2 - y) * s / 20);
			ev(EV_SYN, SYN_REPORT, 0);
		}
	}
	usleep(60000);
	ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
	ev(EV_KEY, BTN_TOUCH, 0);
	ev(EV_SYN, SYN_REPORT, 0);
	return 0;
}
