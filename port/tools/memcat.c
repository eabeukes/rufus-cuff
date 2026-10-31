/* memcat ADDR LEN: copy physical memory to stdout through /dev/mem (page-aligned ADDR). */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	uint64_t addr = strtoull(argv[1], 0, 0);
	size_t len = strtoul(argv[2], 0, 0);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *p = mmap(0, len, PROT_READ, MAP_SHARED, fd, addr);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	for (size_t i = 0; i < len / 4; i++) {
		uint32_t v = p[i];
		fwrite(&v, 4, 1, stdout);
	}
	return 0;
}
