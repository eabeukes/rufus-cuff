/*
 * Map a file inside Android's /data as a block device, without mounting /data read-write.
 *
 *   mapfile print FILE        print FILE's extents ("size BYTES", then "LSECT LEN PSECT" per
 *                             extent, 512-byte sectors, physical = offset in the partition)
 *   mapfile create NAME DEV   read such a map on stdin, create device-mapper linear NAME over
 *                             DEV; prints the new /dev/dm-N
 *
 * Android runs "print" when the image is installed (its own ext4 is authoritative) and keeps
 * the output next to the image. Linux runs "print" again on a read-only mount, refuses to go
 * on unless both agree, unmounts /data and then runs "create". Linux only ever writes to the
 * blocks of that one file, which Android never touches.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_EXTENTS 4096
/* Extents that are not plain, block-aligned, allocated data on the device. */
#define BAD_FLAGS (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED | \
		   FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | \
		   FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN)

static int die(const char *what)
{
	fprintf(stderr, "mapfile: %s: %s\n", what, errno ? strerror(errno) : "invalid");
	return 1;
}

static int print_map(const char *path)
{
	size_t sz = sizeof(struct fiemap) + MAX_EXTENTS * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, sz);
	uint64_t next = 0;
	struct stat st;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) || !fm)
		return die(path);
	if (st.st_size == 0 || st.st_size % 4096) {
		errno = 0;
		return die("size must be a non-zero multiple of 4096");
	}
	fsync(fd);
	fm->fm_start = 0;
	fm->fm_length = st.st_size;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = MAX_EXTENTS;
	if (ioctl(fd, FS_IOC_FIEMAP, fm))
		return die("FIEMAP");

	printf("size %lld\n", (long long)st.st_size);
	for (unsigned int i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];
		uint64_t len = e->fe_length;

		errno = 0;
		if (e->fe_flags & BAD_FLAGS)
			return die("extent not plain allocated data");
		if (e->fe_logical != next || e->fe_logical % 512 || e->fe_physical % 512 || len % 512)
			return die("hole or unaligned extent");
		if (e->fe_logical + len > (uint64_t)st.st_size)
			len = st.st_size - e->fe_logical;
		printf("%llu %llu %llu\n", (unsigned long long)(e->fe_logical / 512),
		       (unsigned long long)(len / 512), (unsigned long long)(e->fe_physical / 512));
		next = e->fe_logical + len;
		if ((e->fe_flags & FIEMAP_EXTENT_LAST) && next != (uint64_t)st.st_size)
			break;
	}
	errno = 0;
	if (next != (uint64_t)st.st_size)
		return die("extents do not cover the whole file");
	return 0;
}

/* Header only: for DM_TABLE_LOAD the target specs follow it in the same buffer. */
static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
	memset(io, 0, sizeof(*io));
	io->version[0] = DM_VERSION_MAJOR;
	io->version[1] = 0;
	io->version[2] = 0;
	io->data_size = size;
	io->data_start = sizeof(*io);
	snprintf(io->name, sizeof(io->name), "%s", name);
}

static int create(const char *name, const char *dev)
{
	static char buf[sizeof(struct dm_ioctl) + MAX_EXTENTS * (sizeof(struct dm_target_spec) + 64)];
	struct dm_ioctl *io = (struct dm_ioctl *)buf;
	unsigned long long lsect, len, psect, size, next = 0, dev_sectors;
	char *p = buf + sizeof(*io);
	unsigned int n = 0;
	struct stat st;
	__u64 dm_dev;
	int ctl, dfd;

	if (stat(dev, &st) || !S_ISBLK(st.st_mode))
		return die(dev);
	dfd = open(dev, O_RDONLY);
	if (dfd < 0 || ioctl(dfd, BLKGETSIZE64, &dev_sectors))
		return die(dev);
	close(dfd);
	dev_sectors /= 512;

	errno = 0;
	if (scanf(" size %llu", &size) != 1 || !size || size % 4096)
		return die("map header");
	while (scanf("%llu %llu %llu", &lsect, &len, &psect) == 3) {
		struct dm_target_spec *t = (struct dm_target_spec *)p;
		char *params = p + sizeof(*t);
		size_t plen;

		if (lsect != next || !len || psect + len > dev_sectors || n == MAX_EXTENTS)
			return die("map extent");
		memset(t, 0, sizeof(*t));
		t->sector_start = lsect;
		t->length = len;
		strcpy(t->target_type, "linear");
		plen = sprintf(params, "%u:%u %llu", major(st.st_rdev), minor(st.st_rdev), psect) + 1;
		t->next = (sizeof(*t) + plen + 7) & ~7UL;
		p += t->next;
		next += len;
		n++;
	}
	if (next * 512 != size)
		return die("map does not cover the file");

	ctl = open("/dev/mapper/control", O_RDWR);
	if (ctl < 0)
		return die("/dev/mapper/control");

	dm_init(io, sizeof(*io), name);
	if (ioctl(ctl, DM_DEV_CREATE, io))
		return die("DM_DEV_CREATE");
	dm_dev = io->dev;

	dm_init(io, p - buf, name);
	io->target_count = n;
	if (ioctl(ctl, DM_TABLE_LOAD, io))
		return die("DM_TABLE_LOAD");

	dm_init(io, sizeof(*io), name);
	if (ioctl(ctl, DM_DEV_SUSPEND, io))	/* without DM_SUSPEND_FLAG: resume */
		return die("DM_DEV_SUSPEND");

	printf("/dev/dm-%u\n", minor(dm_dev));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "print"))
		return print_map(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "create"))
		return create(argv[2], argv[3]);
	fprintf(stderr, "usage: mapfile print FILE | mapfile create NAME DEV < MAP\n");
	return 2;
}
