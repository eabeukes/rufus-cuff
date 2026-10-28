/*
 * Run the vibration motor through its force-feedback input device (regulator-haptic).
 *   vibrate [MS]      buzz for MS milliseconds (default 300)
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The first input device that can rumble. */
static int find(void)
{
	char path[300];
	unsigned char bits[FF_MAX / 8 + 1];
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
		if (ioctl(f, EVIOCGBIT(EV_FF, sizeof(bits)), bits) > 0 && (bits[FF_RUMBLE / 8] >> (FF_RUMBLE % 8) & 1))
			return f;
		close(f);
	}
	return -1;
}

int main(int argc, char **argv)
{
	int ms = argc > 1 ? atoi(argv[1]) : 300;
	struct ff_effect fx = { .type = FF_RUMBLE, .id = -1 };
	struct input_event play = { .type = EV_FF, .value = 1 };
	int fd = find();

	if (fd < 0) {
		fprintf(stderr, "no vibrator\n");
		return 1;
	}
	if (ms <= 0 || ms > 10000) {
		fprintf(stderr, "usage: vibrate [MS] (1-10000)\n");
		return 2;
	}
	fx.u.rumble.strong_magnitude = 0xffff;
	fx.replay.length = ms;
	if (ioctl(fd, EVIOCSFF, &fx) < 0) {
		perror("EVIOCSFF");
		return 1;
	}
	play.code = fx.id;
	if (write(fd, &play, sizeof(play)) != sizeof(play)) {
		perror("play");
		return 1;
	}
	usleep(ms * 1000 + 50000);	/* the effect stops by itself; stay until it has */
	return 0;
}
