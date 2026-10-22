/*
 * pinfile: reserve a fixed-position file in Android's f2fs /data and report where its
 * blocks are on the userdata partition. Used to check whether an iOS root image can live
 * inside userdata without repartitioning (the ios-fuxi approach, patch 0088).
 *
 *   pinfile create  PATH SIZE_MB   new file, pinned (f2fs never moves it), space allocated
 *   pinfile status  PATH           pinned or not, size
 *   pinfile extents PATH [--json]  block positions (FIEMAP), partition-relative bytes
 *   pinfile compare PATH [DEV]     read-only: file bytes through the filesystem vs raw
 *                                  bytes of DEV (default userdata) at the same positions
 *   pinfile rawtest PATH DEV N --yes
 *                                  write a test pattern into the first N 4 KiB blocks of the
 *                                  file (N = all for every block) on DEV, bypassing the
 *                                  filesystem, then read them back from disk
 *   pinfile rawcheck PATH [DEV]    read-only: which blocks carry the test pattern on disk
 *   pinfile fsread  PATH           read-only: drop the file's cached pages and read it through
 *                                  the filesystem; blocks written raw no longer read as zeros
 *                                  (the filesystem decrypts our plaintext into noise), which
 *                                  proves the raw positions are this file's own blocks
 *
 * rawtest is the only command that writes to a raw block device. Before writing it
 * checks (checked_layout): the file is pinned; it is a regular file whose size is a
 * whole number of 4 KiB blocks; its extents start at 0, are contiguous, cover exactly
 * the file and carry no flag other than MERGED/LAST; DEV is the only device under the
 * filesystem's device-mapper device and has the same size (a 1:1 mapping, offset 0);
 * every extent lies inside DEV; no extent overlaps a region of DEV that another
 * device-mapper device maps directly (Xiaomi's `cust` is a linear map inside userdata).
 * Every single write is checked again against the extent list (inside_one_extent)
 * right before it is issued.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define F2FS_IOCTL_MAGIC      0xf5
#define F2FS_IOC_SET_PIN_FILE _IOW(F2FS_IOCTL_MAGIC, 13, __u32)
#define F2FS_IOC_GET_PIN_FILE _IOR(F2FS_IOCTL_MAGIC, 14, __u32)

#define BLK        4096u
#define CHUNK      (1u << 20)
#define MAX_EXT    4096u
#define DEFAULT_DEV "/dev/block/by-name/userdata"

static int die(const char *what)
{
	fprintf(stderr, "pinfile: %s: %s\n", what, strerror(errno));
	return 1;
}

struct ext { uint64_t logical, physical, length; uint32_t flags; };

/* All extents of fd. Returns count, or -1. */
static int get_extents(int fd, struct ext *out, unsigned max)
{
	size_t sz = sizeof(struct fiemap) + max * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, sz);
	if (!fm)
		return -1;
	fm->fm_start = 0;
	fm->fm_length = FIEMAP_MAX_OFFSET;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = max;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
		free(fm);
		return -1;
	}
	int n = (int)fm->fm_mapped_extents;
	for (int i = 0; i < n; i++) {
		out[i].logical = fm->fm_extents[i].fe_logical;
		out[i].physical = fm->fm_extents[i].fe_physical;
		out[i].length = fm->fm_extents[i].fe_length;
		out[i].flags = fm->fm_extents[i].fe_flags;
	}
	free(fm);
	return n;
}

static int cmd_create(const char *path, long mb)
{
	if (mb <= 0 || mb > 65536 || mb % 2) {
		fprintf(stderr, "pinfile: size must be an even number of MiB, 2..65536\n");
		return 1;
	}
	uint64_t size = (uint64_t)mb << 20;
	int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return die("create (refusing to reuse an existing file)");
	__u32 pin = 1;
	if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		errno = e;
		return die("F2FS_IOC_SET_PIN_FILE (file removed)");
	}
	if (fallocate(fd, 0, 0, (off_t)size) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		errno = e;
		return die("fallocate (file removed)");
	}
	/* Write zeros through the filesystem so every block is really allocated
	 * (Android's libfiemap does the same before reading the extents). */
	char *zero = calloc(1, CHUNK);
	if (!zero)
		return die("calloc");
	for (uint64_t off = 0; off < size; off += CHUNK) {
		if (pwrite(fd, zero, CHUNK, (off_t)off) != (ssize_t)CHUNK) {
			free(zero);
			return die("pwrite zeros");
		}
	}
	free(zero);
	if (fsync(fd) < 0)
		return die("fsync");
	close(fd);
	printf("created %s, %ld MiB, pinned\n", path, mb);
	return 0;
}

static int cmd_status(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return die("open");
	/* f2fs reports a pinned file as FS_NOCOW_FL; F2FS_IOC_GET_PIN_FILE returns how
	 * often GC failed to move it, which is 0 for a pinned file that GC never touched
	 * (fs/f2fs/file.c, f2fs_fileattr_get / f2fs_ioc_get_pin_file). */
	__u32 gc_failures = 0;
	int flags = 0;
	struct stat st;
	if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0)
		return die("FS_IOC_GETFLAGS");
	if (ioctl(fd, F2FS_IOC_GET_PIN_FILE, &gc_failures) < 0)
		return die("F2FS_IOC_GET_PIN_FILE");
	if (fstat(fd, &st) < 0)
		return die("fstat");
	printf("%s: %s (GC move failures %u), size %lld bytes, blocks %lld\n", path,
	    (flags & FS_NOCOW_FL) ? "pinned" : "NOT pinned", gc_failures,
	    (long long)st.st_size, (long long)st.st_blocks);
	close(fd);
	return 0;
}

static int cmd_extents(const char *path, int json)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return die("open");
	static struct ext e[MAX_EXT];
	int n = get_extents(fd, e, MAX_EXT);
	if (n < 0)
		return die("FS_IOC_FIEMAP");
	struct stat st;
	fstat(fd, &st);
	close(fd);
	if (json) {
		printf("{\"file\": \"%s\", \"size\": %lld, \"extents\": [", path, (long long)st.st_size);
		for (int i = 0; i < n; i++)
			printf("%s[%llu, %llu, %llu, %u]", i ? ", " : "", (unsigned long long)e[i].logical,
			    (unsigned long long)e[i].physical, (unsigned long long)e[i].length, e[i].flags);
		printf("]}\n");
		return 0;
	}
	printf("%d extents (bytes: logical physical length flags)\n", n);
	for (int i = 0; i < n; i++)
		printf("%4d %016llx %016llx %016llx 0x%x\n", i, (unsigned long long)e[i].logical,
		    (unsigned long long)e[i].physical, (unsigned long long)e[i].length, e[i].flags);
	return 0;
}

/* Read-only: compare each 4 KiB block read through the filesystem with the raw
 * bytes at the extent's physical position on dev. */
static int cmd_compare(const char *path, const char *dev)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return die("open file");
	int raw = open(dev, O_RDONLY | O_CLOEXEC);
	if (raw < 0)
		return die("open device (read-only)");
	static struct ext e[MAX_EXT];
	int n = get_extents(fd, e, MAX_EXT);
	if (n < 0)
		return die("FS_IOC_FIEMAP");
	char a[BLK], b[BLK];
	uint64_t same = 0, differ = 0, raw_zero = 0;
	for (int i = 0; i < n; i++) {
		for (uint64_t off = 0; off + BLK <= e[i].length; off += BLK) {
			if (pread(fd, a, BLK, (off_t)(e[i].logical + off)) != BLK ||
			    pread(raw, b, BLK, (off_t)(e[i].physical + off)) != BLK)
				return die("pread");
			if (memcmp(a, b, BLK) == 0)
				same++;
			else
				differ++;
			int z = 1;
			for (unsigned k = 0; k < BLK; k++)
				if (b[k]) { z = 0; break; }
			raw_zero += z;
		}
	}
	close(fd);
	close(raw);
	printf("blocks: %llu identical, %llu different; raw blocks that are all zero: %llu\n",
	    (unsigned long long)same, (unsigned long long)differ, (unsigned long long)raw_zero);
	return 0;
}

/* ---- raw writes ---------------------------------------------------------------- */

static struct ext L[MAX_EXT];   /* the checked layout used by rawtest/rawcheck */
static int nL;

static int fail(const char *msg)
{
	fprintf(stderr, "pinfile: refused: %s\n", msg);
	return 1;
}

/* Read a decimal number from a sysfs file. */
static int sysfs_u64(const char *path, uint64_t *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	unsigned long long v;
	int ok = fscanf(f, "%llu", &v) == 1;
	fclose(f);
	*out = v;
	return ok ? 0 : -1;
}

/*
 * Every device-mapper device other than the filesystem's own that sits on DEV (sysfs
 * holders) must map DEV only through `linear` targets, and none of those regions may
 * overlap the layout L. DM_TABLE_STATUS with DM_STATUS_TABLE_FLAG returns the table;
 * for status, dm_target_spec.next counts from the first spec.
 */
static int check_other_holders(const char *devname, dev_t devrdev, dev_t fsdev)
{
	char hp[PATH_MAX];
	snprintf(hp, sizeof hp, "/sys/class/block/%s/holders", devname);
	DIR *d = opendir(hp);
	if (!d)
		return fail("cannot list DEV's holders");
	int ctl = open("/dev/device-mapper", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		closedir(d);
		return die("open /dev/device-mapper");
	}
	int others = 0, rc = 0;
	for (struct dirent *h; !rc && (h = readdir(d));) {
		if (h->d_name[0] == '.')
			continue;
		char dp[PATH_MAX];
		unsigned hmaj, hmin;
		snprintf(dp, sizeof dp, "/sys/class/block/%s/dev", h->d_name);
		FILE *f = fopen(dp, "r");
		if (!f || fscanf(f, "%u:%u", &hmaj, &hmin) != 2) {
			if (f)
				fclose(f);
			rc = fail("cannot read a holder's device number");
			break;
		}
		fclose(f);
		if (makedev(hmaj, hmin) == fsdev)
			continue;
		others++;
		static char buf[65536] __attribute__((aligned(8)));
		struct dm_ioctl *io = (struct dm_ioctl *)buf;
		memset(buf, 0, sizeof buf);
		io->version[0] = DM_VERSION_MAJOR;
		io->data_size = sizeof buf;
		io->data_start = sizeof(struct dm_ioctl);
		io->dev = makedev(hmaj, hmin);
		io->flags = DM_STATUS_TABLE_FLAG;
		if (ioctl(ctl, DM_TABLE_STATUS, io) < 0) {
			rc = die("DM_TABLE_STATUS");
			break;
		}
		if (io->flags & DM_BUFFER_FULL_FLAG) {
			rc = fail("a holder's table does not fit the buffer");
			break;
		}
		const char *first = buf + io->data_start;
		uint32_t at = 0;
		for (uint32_t t = 0; !rc && t < io->target_count; t++) {
			const struct dm_target_spec *sp = (const struct dm_target_spec *)(first + at);
			const char *params = (const char *)(sp + 1);
			unsigned pmaj, pmin;
			unsigned long long off;
			if (strncmp(sp->target_type, "linear", DM_MAX_TYPE_NAME) != 0 ||
			    sscanf(params, "%u:%u %llu", &pmaj, &pmin, &off) != 3) {
				fprintf(stderr, "pinfile: holder %s target %u: %.16s %s\n", h->d_name, t,
				    sp->target_type, params);
				rc = fail("a holder of DEV maps it with something other than linear");
				break;
			}
			if (makedev(pmaj, pmin) == devrdev) {
				uint64_t start = (uint64_t)off * 512, len = sp->length * 512;
				printf("holder %s maps %s bytes [%#llx, %#llx)\n", h->d_name, devname,
				    (unsigned long long)start, (unsigned long long)(start + len));
				for (int i = 0; i < nL; i++)
					if (L[i].physical < start + len && start < L[i].physical + L[i].length)
						rc = fail("the file overlaps a region another device maps directly");
			}
			at = sp->next;
		}
	}
	closedir(d);
	close(ctl);
	if (!rc)
		printf("other holders of %s: %d, no overlap\n", devname, others);
	return rc;
}

/* All checks that must hold before anything is written; fills L/nL. */
static int checked_layout(const char *path, const char *dev, int devfd)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return die("open file");
	int flags = 0;
	struct stat st;
	if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0 || fstat(fd, &st) < 0)
		return die("file flags/stat");
	if (!(flags & FS_NOCOW_FL))
		return fail("file is not pinned");
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % BLK)
		return fail("not a regular file of whole 4 KiB blocks");
	nL = get_extents(fd, L, MAX_EXT);
	close(fd);
	if (nL <= 0 || nL >= (int)MAX_EXT)
		return fail("no extent list (or too many extents)");
	uint64_t next = 0;
	for (int i = 0; i < nL; i++) {
		/* DATA_ENCRYPTED only says the filesystem encrypts the contents (FBE); the
		 * block positions are what matter here. fiemap_fill_next_extent() adds ENCODED
		 * to every DATA_ENCRYPTED extent, so ENCODED is accepted only together with it. */
		uint32_t ok = FIEMAP_EXTENT_MERGED | FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_DATA_ENCRYPTED;
		if (L[i].flags & FIEMAP_EXTENT_DATA_ENCRYPTED)
			ok |= FIEMAP_EXTENT_ENCODED;
		uint32_t bad = L[i].flags & ~ok;
		if (bad)
			return fail("extent with an unexpected flag (unwritten/encoded/inline/...)");
		if (L[i].logical != next || L[i].logical % BLK || L[i].physical % BLK ||
		    L[i].length % BLK || !L[i].length)
			return fail("extents are not contiguous, aligned 4 KiB runs");
		next += L[i].length;
	}
	if (next != (uint64_t)st.st_size)
		return fail("extents do not cover exactly the file");

	/* The filesystem's device must be a device-mapper device whose only slave is DEV,
	 * with DEV's size: then the mapping is 1:1 from offset 0 and FIEMAP's physical
	 * offsets are offsets on DEV. */
	char real[PATH_MAX], sys[PATH_MAX];
	struct stat dst;
	if (!realpath(dev, real) || fstat(devfd, &dst) < 0 || !S_ISBLK(dst.st_mode))
		return fail("DEV is not a block device");
	unsigned fmaj = major(st.st_dev), fmin = minor(st.st_dev);
	snprintf(sys, sizeof sys, "/sys/dev/block/%u:%u/slaves", fmaj, fmin);
	char *want = basename(real);
	char slave[PATH_MAX];
	snprintf(slave, sizeof slave, "%s/%s", sys, want);
	struct stat sl;
	if (stat(slave, &sl) < 0)
		return fail("DEV is not the device under this filesystem");
	int nslaves = 0;
	DIR *d = opendir(sys);
	if (!d)
		return fail("cannot list the filesystem device's slaves");
	for (struct dirent *e; (e = readdir(d));)
		if (e->d_name[0] != '.')
			nslaves++;
	closedir(d);
	if (nslaves != 1)
		return fail("the filesystem device does not have exactly one slave");
	char dmname[PATH_MAX];
	snprintf(dmname, sizeof dmname, "/sys/dev/block/%u:%u/dm/name", fmaj, fmin);
	if (stat(dmname, &sl) < 0)
		return fail("the filesystem device is not a device-mapper device");
	uint64_t fsz, dsz;
	char path2[PATH_MAX];
	snprintf(path2, sizeof path2, "/sys/dev/block/%u:%u/size", fmaj, fmin);
	if (sysfs_u64(path2, &fsz) < 0)
		return fail("cannot read filesystem device size");
	snprintf(path2, sizeof path2, "/sys/dev/block/%u:%u/size", major(dst.st_rdev), minor(dst.st_rdev));
	if (sysfs_u64(path2, &dsz) < 0 || fsz != dsz)
		return fail("filesystem device and DEV differ in size");
	uint64_t bytes = 0;
	if (ioctl(devfd, BLKGETSIZE64, &bytes) < 0)
		return die("BLKGETSIZE64");
	for (int i = 0; i < nL; i++)
		if (L[i].physical + L[i].length > bytes)
			return fail("an extent lies beyond the end of DEV");
	if (check_other_holders(want, dst.st_rdev, st.st_dev))
		return 1;
	printf("checked: pinned, %lld bytes, %d extents, all inside %s (%s)\n",
	    (long long)st.st_size, nL, dev, real);
	return 0;
}

/* True only if [off, off+len) lies entirely inside one extent of the checked layout. */
static int inside_one_extent(uint64_t off, uint64_t len)
{
	for (int i = 0; i < nL; i++)
		if (off >= L[i].physical && len <= L[i].length &&
		    off - L[i].physical <= L[i].length - len)
			return 1;
	return 0;
}

/* Test pattern of the 4 KiB block at file offset `logical`. */
static void pattern(unsigned char *b, uint64_t logical, uint64_t physical)
{
	memset(b, 0, BLK);
	int n = snprintf((char *)b, 64, "IOSPANDORA RAWTEST v1 L=%016llx P=%016llx\n",
	    (unsigned long long)logical, (unsigned long long)physical);
	uint64_t x = logical * 6364136223846793005ull + 1442695040888963407ull;
	for (unsigned k = (unsigned)n; k < BLK; k++) {
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		b[k] = (unsigned char)(x >> 56);
	}
}

/* Compare every block of the layout on disk (O_DIRECT, no page cache) with the pattern. */
static int verify(int rfd, uint64_t *good, uint64_t *badc)
{
	unsigned char *buf, *want;
	if (posix_memalign((void **)&buf, BLK, BLK) || posix_memalign((void **)&want, BLK, BLK))
		return -1;
	*good = *badc = 0;
	for (int i = 0; i < nL; i++)
		for (uint64_t off = 0; off < L[i].length; off += BLK) {
			if (pread(rfd, buf, BLK, (off_t)(L[i].physical + off)) != BLK)
				return -1;
			pattern(want, L[i].logical + off, L[i].physical + off);
			if (memcmp(buf, want, BLK) == 0)
				(*good)++;
			else
				(*badc)++;
		}
	free(buf);
	free(want);
	return 0;
}

static int cmd_rawtest(const char *path, const char *dev, const char *count)
{
	uint64_t limit = UINT64_MAX;
	if (strcmp(count, "all") != 0) {
		char *end;
		limit = strtoull(count, &end, 10);
		if (*end || limit == 0)
			return fail("N must be a positive number of blocks or 'all'");
	}
	int wfd = open(dev, O_RDWR | O_DIRECT | O_SYNC | O_CLOEXEC);
	if (wfd < 0)
		return die("open DEV for writing");
	if (checked_layout(path, dev, wfd))
		return 1;
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, BLK))
		return die("posix_memalign");
	uint64_t written = 0;
	for (int i = 0; i < nL && written < limit; i++)
		for (uint64_t off = 0; off < L[i].length && written < limit; off += BLK) {
			uint64_t phys = L[i].physical + off;
			if (!inside_one_extent(phys, BLK))
				return fail("internal: write outside the extents");
			pattern(b, L[i].logical + off, phys);
			if (pwrite(wfd, b, BLK, (off_t)phys) != BLK)
				return die("pwrite");
			written++;
		}
	if (fsync(wfd) < 0)
		return die("fsync DEV");
	close(wfd);
	free(b);
	int rfd = open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
	uint64_t good, badc;
	if (rfd < 0 || verify(rfd, &good, &badc) < 0)
		return die("read back");
	close(rfd);
	printf("wrote %llu blocks; read back from disk: %llu carry the pattern, %llu do not\n",
	    (unsigned long long)written, (unsigned long long)good, (unsigned long long)badc);
	return good == written ? 0 : 1;
}

static int cmd_rawcheck(const char *path, const char *dev)
{
	int rfd = open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (rfd < 0)
		return die("open DEV (read-only)");
	if (checked_layout(path, dev, rfd))
		return 1;
	uint64_t good, badc;
	if (verify(rfd, &good, &badc) < 0)
		return die("read");
	close(rfd);
	printf("on disk: %llu blocks carry the test pattern, %llu do not\n",
	    (unsigned long long)good, (unsigned long long)badc);
	return badc ? 1 : 0;
}

/* Read-only through the filesystem: which 4 KiB blocks are no longer all zero. */
static int cmd_fsread(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return die("open");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0)
		return die("posix_fadvise");
	struct stat st;
	if (fstat(fd, &st) < 0)
		return die("fstat");
	char b[BLK];
	uint64_t nonzero = 0, first = UINT64_MAX, last = 0;
	for (uint64_t off = 0; off + BLK <= (uint64_t)st.st_size; off += BLK) {
		if (pread(fd, b, BLK, (off_t)off) != BLK)
			return die("pread");
		for (unsigned k = 0; k < BLK; k++)
			if (b[k]) {
				nonzero++;
				if (first == UINT64_MAX)
					first = off / BLK;
				last = off / BLK;
				break;
			}
	}
	close(fd);
	printf("through the filesystem: %llu of %llu blocks are not zero",
	    (unsigned long long)nonzero, (unsigned long long)(st.st_size / BLK));
	if (nonzero)
		printf(" (blocks %llu..%llu)", (unsigned long long)first, (unsigned long long)last);
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "rawtest") && !strcmp(argv[5], "--yes"))
		return cmd_rawtest(argv[2], argv[3], argv[4]);
	if (argc == 3 && !strcmp(argv[1], "fsread"))
		return cmd_fsread(argv[2]);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "rawcheck"))
		return cmd_rawcheck(argv[2], argc == 4 ? argv[3] : DEFAULT_DEV);
	if (argc >= 4 && !strcmp(argv[1], "create"))
		return cmd_create(argv[2], strtol(argv[3], NULL, 10));
	if (argc == 3 && !strcmp(argv[1], "status"))
		return cmd_status(argv[2]);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "extents"))
		return cmd_extents(argv[2], argc == 4 && !strcmp(argv[3], "--json"));
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "compare"))
		return cmd_compare(argv[2], argc == 4 ? argv[3] : DEFAULT_DEV);
	fprintf(stderr,
	    "usage: pinfile create PATH SIZE_MB | status PATH | extents PATH [--json] | compare PATH [DEV]\n"
	    "       pinfile rawtest PATH DEV N|all --yes | rawcheck PATH [DEV] | fsread PATH\n");
	return 2;
}
