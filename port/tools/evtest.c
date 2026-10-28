/*
 * evtest: print input events from every /dev/input/event* device.
 *   evtest [SECONDS]      (default 15) - lists devices, then prints "dev type code value"
 * Each event line also goes to /dev/kmsg, so it survives a reboot into Android via pstore.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 15;
	struct pollfd fds[16];
	char names[16][64];
	int n = 0;
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	while (d && (e = readdir(d)) && n < 16) {
		char path[300];
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fds[n].fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fds[n].fd < 0)
			continue;
		fds[n].events = POLLIN;
		names[n][0] = 0;
		ioctl(fds[n].fd, EVIOCGNAME(sizeof(names[n])), names[n]);
		printf("%s: %s\n", e->d_name, names[n]);
		n++;
	}
	fflush(stdout);

	FILE *kmsg = fopen("/dev/kmsg", "w");
	time_t end = time(NULL) + secs;
	while (time(NULL) < end) {
		if (poll(fds, n, 500) <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			struct input_event ev;
			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_SYN)
					continue;
				printf("[%s] type=%u code=%u value=%d\n", names[i], ev.type, ev.code, ev.value);
				if (kmsg) {
					fprintf(kmsg, "evtest: [%s] type=%u code=%u value=%d\n",
						names[i], ev.type, ev.code, ev.value);
					fflush(kmsg);
				}
			}
		}
		fflush(stdout);
	}
	printf("evtest done\n");
	return 0;
}
