/*
 * disptool: poke the MT6739 display hardware and memory through /dev/mem.
 *
 *   disptool r ADDR [COUNT]              read COUNT 32-bit words
 *   disptool w ADDR VAL [VAL...]         write consecutive words
 *   disptool dump ADDR BYTES             hex dump, 8 words per line, skips all-zero lines
 *   disptool fill ADDR WORDS VAL         fill WORDS words with VAL
 *   disptool pattern ADDR [W H STRIDE]   orientation test image (default 480x640, stride 1920):
 *                                        quadrants R/G/B/white, white border, black arrow from
 *                                        the centre towards row 0 / column 0
 *   disptool cmp ADDR WORDS VAL          count words that differ from VAL
 *
 * Accesses are uncached (O_SYNC). Addresses and values accept 0x prefixes.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int memfd = -1;

static volatile uint32_t *map(uint64_t addr, size_t len, void **base, size_t *maplen)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint64_t start = addr & ~(uint64_t)(pg - 1);

	if (memfd < 0) {
		memfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (memfd < 0) {
			perror("/dev/mem");
			exit(1);
		}
	}
	*maplen = (addr - start) + len;
	*base = mmap(NULL, *maplen, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, start);
	if (*base == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return (volatile uint32_t *)((char *)*base + (addr - start));
}

static uint64_t num(const char *s)
{
	return strtoull(s, NULL, 0);
}

static void put(volatile uint32_t *fb, uint32_t stride_w, uint32_t x, uint32_t y, uint32_t c)
{
	fb[y * stride_w + x] = c;
}

int main(int argc, char **argv)
{
	void *base;
	size_t len;

	if (argc < 3) {
		fprintf(stderr, "usage: see source header\n");
		return 2;
	}
	const char *cmd = argv[1];
	uint64_t addr = num(argv[2]);

	if (!strcmp(cmd, "r")) {
		uint32_t n = argc > 3 ? num(argv[3]) : 1;
		volatile uint32_t *p = map(addr, n * 4, &base, &len);
		for (uint32_t i = 0; i < n; i++)
			printf("0x%08llx: 0x%08x\n", (unsigned long long)(addr + i * 4), p[i]);
	} else if (!strcmp(cmd, "w")) {
		int n = argc - 3;
		volatile uint32_t *p = map(addr, n * 4, &base, &len);
		for (int i = 0; i < n; i++)
			p[i] = num(argv[3 + i]);
		__sync_synchronize();
	} else if (!strcmp(cmd, "dump")) {
		uint32_t bytes = num(argv[3]);
		volatile uint32_t *p = map(addr, bytes, &base, &len);
		for (uint32_t i = 0; i < bytes / 4; i += 8) {
			uint32_t w[8], any = 0;
			for (int j = 0; j < 8 && i + j < bytes / 4; j++)
				any |= w[j] = p[i + j];
			if (!any)
				continue;
			printf("%08llx:", (unsigned long long)(addr + i * 4));
			for (int j = 0; j < 8 && i + j < bytes / 4; j++)
				printf(" %08x", w[j]);
			printf("\n");
		}
	} else if (!strcmp(cmd, "fill") || !strcmp(cmd, "cmp")) {
		uint32_t n = num(argv[3]), v = num(argv[4]), diff = 0;
		volatile uint32_t *p = map(addr, n * 4, &base, &len);
		if (cmd[0] == 'f') {
			for (uint32_t i = 0; i < n; i++)
				p[i] = v;
			__sync_synchronize();
		} else {
			for (uint32_t i = 0; i < n; i++)
				diff += p[i] != v;
			printf("%u of %u words differ from 0x%08x\n", diff, n, v);
		}
	} else if (!strcmp(cmd, "pattern")) {
		uint32_t w = argc > 3 ? num(argv[3]) : 480, h = argc > 4 ? num(argv[4]) : 640;
		uint32_t stride = argc > 5 ? num(argv[5]) : 1920, sw = stride / 4;
		volatile uint32_t *fb = map(addr, stride * h, &base, &len);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				uint32_t c = y < h / 2 ? (x < w / 2 ? 0xffff0000 : 0xff00ff00)
						       : (x < w / 2 ? 0xff0000ff : 0xffffffff);
				if (x < 8 || y < 8 || x >= w - 8 || y >= h - 8)
					c = 0xffffffff;
				put(fb, sw, x, y, c);
			}
		/* arrow: thick line from centre towards (0,0), head at the origin corner */
		for (uint32_t t = 0; t < w / 2; t++)
			for (int d = -6; d <= 6; d++) {
				uint32_t x = w / 2 - t, y = (h / 2) - t * h / w;
				if ((int)x + d >= 0 && (int)x + d < (int)w)
					put(fb, sw, x + d, y, 0xff000000);
			}
		for (uint32_t i = 0; i < 60; i++)
			for (uint32_t j = 0; j < 60 - i; j++)
				put(fb, sw, 20 + i, 20 + j, 0xff000000);
		__sync_synchronize();
	} else {
		fprintf(stderr, "unknown command %s\n", cmd);
		return 2;
	}
	return 0;
}
