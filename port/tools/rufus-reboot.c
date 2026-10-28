/*
 * Reboot with a reboot-mode command, which the RTC driver turns into LK's boot-mode bits:
 *   rufus-reboot recovery      LK boots the recovery partition once
 *   rufus-reboot bootloader    LK's fastboot
 * No shutdown of its own: run it last (inittab's shutdown step, after OpenRC).
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "recovery") && strcmp(argv[1], "bootloader"))) {
		fprintf(stderr, "usage: rufus-reboot recovery|bootloader\n");
		return 2;
	}
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, argv[1]);
	perror("reboot");
	return 1;
}
