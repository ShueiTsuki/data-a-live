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
 *   pinfile rawwrite PATH DEV N --yes
 *                                  copy standard input (an image) into the first N 4 KiB blocks
 *                                  of the file (N = all) on DEV, bypassing the filesystem; a
 *                                  short last block is zero-padded, input shorter than N stops
 *   pinfile rawhash PATH [DEV] [BYTES]
 *                                  read-only: SHA-256 of the file's first BYTES (default: all)
 *                                  read raw from DEV in file order, to compare with the image
 *   pinfile rawread PATH [DEV] [BYTES]
 *                                  read-only: the file's first BYTES (default: all) read raw
 *                                  from DEV in file order, to standard output (the kernel's
 *                                  log file, written below the encryption layer)
 *   pinfile rawreadat PATH DEV OFFSET LEN
 *                                  read-only: LEN bytes from byte OFFSET of the file (both whole
 *                                  4 KiB blocks) read raw from DEV, to standard output
 *   pinfile rawwriteat PATH DEV OFFSET OLD_SHA256 --yes
 *                                  overwrite 1..64 blocks from byte OFFSET with standard input,
 *                                  raw on DEV, only if those blocks now hash to OLD_SHA256
 *   pinfile fsread  PATH          read-only: drop the file's cached pages and read it through
 *                                  the filesystem; blocks written raw no longer read as zeros
 *                                  (the filesystem decrypts our plaintext into noise), which
 *                                  proves the raw positions are this file's own blocks
 *
 * rawtest, rawwrite and rawwriteat write to a raw block device. Before writing they
 * check (checked_layout): the file is pinned; it is a regular file whose size is a
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
#define MAX_EXT    65536u
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

/* ---- SHA-256 (FIPS 180-4), for rawhash: the binary is static, no libcrypto ---- */
struct sha256 { uint32_t h[8]; uint64_t len; unsigned char buf[64]; unsigned n; };

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *c, const unsigned char *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		    (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++)
		w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] +
		    (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (int i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256 *c)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(c->h, iv, sizeof iv);
	c->len = 0;
	c->n = 0;
}

static void sha256_update(struct sha256 *c, const unsigned char *p, size_t len)
{
	c->len += len;
	while (len) {
		unsigned take = 64 - c->n < len ? 64 - c->n : (unsigned)len;
		memcpy(c->buf + c->n, p, take);
		c->n += take;
		p += take;
		len -= take;
		if (c->n == 64) {
			sha256_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void sha256_final(struct sha256 *c, unsigned char out[32])
{
	uint64_t bits = c->len * 8;
	unsigned char pad = 0x80, zero = 0, lenb[8];
	sha256_update(c, &pad, 1);
	while (c->n != 56)
		sha256_update(c, &zero, 1);
	for (int i = 0; i < 8; i++)
		lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
	sha256_update(c, lenb, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)(c->h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
		out[4 * i + 3] = (unsigned char)c->h[i];
	}
}

/* Read exactly n bytes from fd unless EOF comes first; returns bytes read or -1. */
static ssize_t read_full(int fd, unsigned char *p, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

static int cmd_rawwrite(const char *path, const char *dev, const char *count)
{
	uint64_t limit = UINT64_MAX;
	if (strcmp(count, "all") != 0) {
		char *end;
		limit = strtoull(count, &end, 10);
		if (*end || limit == 0)
			return fail("N must be a positive number of blocks or 'all'");
	}
	int wfd = open(dev, O_RDWR | O_DIRECT | O_CLOEXEC);
	if (wfd < 0)
		return die("open DEV for writing");
	if (checked_layout(path, dev, wfd))
		return 1;
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, CHUNK))
		return die("posix_memalign");
	struct sha256 h;
	sha256_init(&h);
	uint64_t written = 0;
	int eof = 0;
	for (int i = 0; i < nL && written < limit && !eof; i++)
		for (uint64_t off = 0; off < L[i].length && written < limit && !eof;) {
			uint64_t blocks = (L[i].length - off) / BLK;
			if (blocks > CHUNK / BLK)
				blocks = CHUNK / BLK;
			if (blocks > limit - written)
				blocks = limit - written;
			ssize_t got = read_full(0, b, (size_t)(blocks * BLK));
			if (got < 0)
				return die("read standard input");
			if (got == 0)
				break;
			if ((uint64_t)got < blocks * BLK) {
				/* Input ended: zero-pad the last block, write only what holds data. */
				blocks = ((uint64_t)got + BLK - 1) / BLK;
				memset(b + got, 0, (size_t)(blocks * BLK - (uint64_t)got));
				eof = 1;
			}
			uint64_t phys = L[i].physical + off, len = blocks * BLK;
			if (!inside_one_extent(phys, len))
				return fail("internal: write outside the extents");
			if (pwrite(wfd, b, (size_t)len, (off_t)phys) != (ssize_t)len)
				return die("pwrite");
			sha256_update(&h, b, (size_t)len);
			written += blocks;
			off += len;
		}
	if (fsync(wfd) < 0)
		return die("fsync DEV");
	close(wfd);
	free(b);
	unsigned char d[32];
	sha256_final(&h, d);
	printf("wrote %llu blocks (%llu bytes); sha256 of what was written: ",
	    (unsigned long long)written, (unsigned long long)written * BLK);
	for (int i = 0; i < 32; i++)
		printf("%02x", d[i]);
	printf("\n");
	return 0;
}

static int cmd_rawhash(const char *path, const char *dev, const char *bytes_arg)
{
	int rfd = open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (rfd < 0)
		return die("open DEV (read-only)");
	if (checked_layout(path, dev, rfd))
		return 1;
	uint64_t want = UINT64_MAX;
	if (bytes_arg) {
		char *end;
		want = strtoull(bytes_arg, &end, 0);
		if (*end || want == 0)
			return fail("BYTES must be a positive number");
	}
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, CHUNK))
		return die("posix_memalign");
	struct sha256 h;
	sha256_init(&h);
	uint64_t done = 0;
	for (int i = 0; i < nL && done < want; i++)
		for (uint64_t off = 0; off < L[i].length && done < want;) {
			uint64_t n = L[i].length - off < CHUNK ? L[i].length - off : CHUNK;
			if (pread(rfd, b, (size_t)n, (off_t)(L[i].physical + off)) != (ssize_t)n)
				return die("pread");
			uint64_t use = want - done < n ? want - done : n;
			sha256_update(&h, b, (size_t)use);
			done += use;
			off += n;
		}
	close(rfd);
	free(b);
	unsigned char d[32];
	sha256_final(&h, d);
	for (int i = 0; i < 32; i++)
		printf("%02x", d[i]);
	printf("  %s raw, first %llu bytes\n", path, (unsigned long long)done);
	return 0;
}

static int cmd_rawread(const char *path, const char *dev, const char *bytes_arg)
{
	int rfd = open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (rfd < 0)
		return die("open DEV (read-only)");
	/* checked_layout prints a line: keep standard output for the data. */
	int saved = dup(1);
	if (saved < 0 || dup2(2, 1) < 0)
		return die("dup");
	int bad = checked_layout(path, dev, rfd);
	fflush(stdout);
	if (dup2(saved, 1) < 0)
		return die("dup2");
	close(saved);
	if (bad)
		return 1;
	uint64_t want = UINT64_MAX;
	if (bytes_arg) {
		char *end;
		want = strtoull(bytes_arg, &end, 0);
		if (*end || want == 0)
			return fail("BYTES must be a positive number");
	}
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, CHUNK))
		return die("posix_memalign");
	uint64_t done = 0;
	for (int i = 0; i < nL && done < want; i++)
		for (uint64_t off = 0; off < L[i].length && done < want;) {
			uint64_t n = L[i].length - off < CHUNK ? L[i].length - off : CHUNK;
			if (pread(rfd, b, (size_t)n, (off_t)(L[i].physical + off)) != (ssize_t)n)
				return die("pread");
			uint64_t use = want - done < n ? want - done : n;
			if (write(1, b, (size_t)use) != (ssize_t)use)
				return die("write");
			done += use;
			off += n;
		}
	close(rfd);
	free(b);
	return 0;
}

/* Raw position of the file's byte OFF (a block start), or UINT64_MAX. */
static uint64_t phys_of(uint64_t off)
{
	for (int i = 0; i < nL; i++)
		if (off >= L[i].logical && off - L[i].logical < L[i].length)
			return L[i].physical + (off - L[i].logical);
	return UINT64_MAX;
}

static int parse_blocks(const char *s, uint64_t *v, const char *what)
{
	char *end;
	*v = strtoull(s, &end, 0);
	if (*end || *v % BLK) {
		fprintf(stderr, "pinfile: refused: %s must be a multiple of %u\n", what, BLK);
		return 1;
	}
	return 0;
}

/* Read-only: LEN bytes of the file from byte OFF, read raw from DEV, to standard output. */
static int cmd_rawreadat(const char *path, const char *dev, const char *off_arg, const char *len_arg)
{
	uint64_t off, len;
	if (parse_blocks(off_arg, &off, "OFFSET") || parse_blocks(len_arg, &len, "LEN") || !len)
		return 1;
	int rfd = open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (rfd < 0)
		return die("open DEV (read-only)");
	int saved = dup(1);
	if (saved < 0 || dup2(2, 1) < 0)
		return die("dup");
	int bad = checked_layout(path, dev, rfd);
	fflush(stdout);
	if (dup2(saved, 1) < 0)
		return die("dup2");
	close(saved);
	if (bad)
		return 1;
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, BLK))
		return die("posix_memalign");
	for (uint64_t at = off; at < off + len; at += BLK) {
		uint64_t phys = phys_of(at);
		if (phys == UINT64_MAX)
			return fail("OFFSET+LEN beyond the file");
		if (pread(rfd, b, BLK, (off_t)phys) != BLK)
			return die("pread");
		if (write(1, b, BLK) != BLK)
			return die("write");
	}
	close(rfd);
	free(b);
	return 0;
}

/*
 * Overwrite whole blocks of the file from byte OFF with standard input (a multiple of
 * 4 KiB), raw on DEV. OLD is the SHA-256 of those blocks as they are now: anything else on
 * disk and nothing is written. Prints the SHA-256 read back after the write.
 */
static int cmd_rawwriteat(const char *path, const char *dev, const char *off_arg, const char *old)
{
	uint64_t off;
	if (parse_blocks(off_arg, &off, "OFFSET"))
		return 1;
	static unsigned char in[64 * BLK];
	ssize_t got = read_full(0, in, sizeof in);
	if (got < 0)
		return die("read standard input");
	unsigned char extra;
	if (got == 0 || got % BLK || (got == (ssize_t)sizeof in && read_full(0, &extra, 1) != 0))
		return fail("input must be 1..64 whole 4 KiB blocks");
	uint64_t len = (uint64_t)got;
	int fd = open(dev, O_RDWR | O_DIRECT | O_CLOEXEC);
	if (fd < 0)
		return die("open DEV for writing");
	if (checked_layout(path, dev, fd))
		return 1;
	unsigned char *b;
	if (posix_memalign((void **)&b, BLK, BLK))
		return die("posix_memalign");
	char hex[65];
	unsigned char d[32];
	struct sha256 h;
	for (int pass = 0; pass < 2; pass++) {
		sha256_init(&h);
		for (uint64_t at = off; at < off + len; at += BLK) {
			uint64_t phys = phys_of(at);
			if (phys == UINT64_MAX || !inside_one_extent(phys, BLK))
				return fail("OFFSET+LEN beyond the file");
			if (pread(fd, b, BLK, (off_t)phys) != BLK)
				return die("pread");
			sha256_update(&h, b, BLK);
		}
		sha256_final(&h, d);
		for (int i = 0; i < 32; i++)
			sprintf(hex + 2 * i, "%02x", d[i]);
		if (pass == 1)
			break;
		if (strcmp(hex, old) != 0) {
			fprintf(stderr, "pinfile: refused: on disk now %s, expected %s\n", hex, old);
			return 1;
		}
		for (uint64_t at = off; at < off + len; at += BLK) {
			memcpy(b, in + (at - off), BLK);
			if (pwrite(fd, b, BLK, (off_t)phys_of(at)) != BLK)
				return die("pwrite");
		}
		if (fsync(fd) < 0)
			return die("fsync DEV");
	}
	close(fd);
	free(b);
	printf("wrote %llu bytes at %#llx; sha256 read back: %s\n", (unsigned long long)len,
	    (unsigned long long)off, hex);
	return 0;
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
	if (argc == 6 && !strcmp(argv[1], "rawwrite") && !strcmp(argv[5], "--yes"))
		return cmd_rawwrite(argv[2], argv[3], argv[4]);
	if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "rawread"))
		return cmd_rawread(argv[2], argc >= 4 ? argv[3] : DEFAULT_DEV, argc == 5 ? argv[4] : NULL);
	if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "rawhash"))
		return cmd_rawhash(argv[2], argc >= 4 ? argv[3] : DEFAULT_DEV, argc == 5 ? argv[4] : NULL);
	if (argc == 6 && !strcmp(argv[1], "rawreadat"))
		return cmd_rawreadat(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 7 && !strcmp(argv[1], "rawwriteat") && !strcmp(argv[6], "--yes"))
		return cmd_rawwriteat(argv[2], argv[3], argv[4], argv[5]);
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
	    "       pinfile rawtest PATH DEV N|all --yes | rawcheck PATH [DEV] | fsread PATH\n"
	    "       pinfile rawwrite PATH DEV N|all --yes < IMAGE | rawhash PATH [DEV] [BYTES]\n"
	    "       pinfile rawread PATH [DEV] [BYTES] > OUT | rawreadat PATH DEV OFFSET LEN > OUT\n"
	    "       pinfile rawwriteat PATH DEV OFFSET OLD_SHA256 --yes < BLOCKS\n");
	return 2;
}
