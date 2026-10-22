/*
 * Equivalence test for cs_native4k_prepare (bsd/kern/ubc_subr.c): the original 16 KiB-read
 * version (patch 0105) and the 1 MiB-read version (patch 0127) are compiled from the
 * patched tree's own source text, run on real 16K-signed iOS files, and their derived 4K
 * digest arrays are compared byte for byte. A corrupted copy must fail in both.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <openssl/sha.h>

#define PAGE_SHIFT 12
#define EBADEXEC 85
#define PAGE_SIZE 4096
#define PAGE_MASK 4095
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CS_HASH_MAX_SIZE 48
#define CS_SUPPORTSSCATTER 0x20100
#define CS_SUPPORTSCODELIMIT64 0x20300
#define ntohll(x) __builtin_bswap64(x)
#define os_mul_overflow __builtin_mul_overflow
#define os_add_overflow __builtin_add_overflow
#define round_page_overflow(in, out) (*(out) = ((in) + PAGE_MASK) & ~(vm_size_t)PAGE_MASK, 0)
#define os_atomic_inc(p, o) (++*(p))
#define KMA_KOBJECT 1
#define KMA_DATA 2
#define KMA_ZERO 4
#define VM_KERN_MEMORY_SECURITY 0
#define KERN_SUCCESS 0
#define VM_PROT_READ 1
#define TRUE 1
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define Z_WAITOK 0
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef int vnode_t;
typedef char *caddr_t;

typedef struct {
	uint32_t magic, length, version, flags, hashOffset, identOffset, nSpecialSlots, nCodeSlots,
	    codeLimit;
	uint8_t hashSize, hashType, platform, pageSize;
	uint32_t spare2, scatterOffset, teamOffset, spare3;
	uint64_t codeLimit64;
} CS_CodeDirectory;

union cs_hash_union { SHA256_CTX sha256; };
struct cs_hash {
	uint32_t cs_type, cs_size, cs_digest_size;
	void (*cs_init)(union cs_hash_union *);
	void (*cs_update)(union cs_hash_union *, const void *, size_t);
	void (*cs_final)(void *, union cs_hash_union *);
};
static void s_init(union cs_hash_union *c) { SHA256_Init(&c->sha256); }
static void s_update(union cs_hash_union *c, const void *d, size_t n) { SHA256_Update(&c->sha256, d, n); }
static void s_final(void *d, union cs_hash_union *c) { SHA256_Final(d, &c->sha256); }
static const struct cs_hash sha256_hash = { 2, 32, 32, s_init, s_update, s_final };

struct cs_blob {
	const CS_CodeDirectory *csb_cd;
	const struct cs_hash *csb_hashtype;
	unsigned csb_hash_pageshift, csb_hash_firstlevel_pageshift;
	int64_t csb_base_offset;
	const char *csb_mem_kaddr;
	vm_size_t csb_mem_size;
	const uint8_t *csb_native4k_hashes;
	vm_size_t csb_native4k_hashes_size, csb_native4k_hash_count;
};

static unsigned long cs_validate_page_bad_hash, cs_native4k_full_blocks, cs_native4k_ready_blobs;
static unsigned long reads;
static int kernel_map;
static int g_fd;

static const unsigned char *
hashes(const CS_CodeDirectory *cd, uint32_t page, size_t hash_len, const char *lower, const char *upper)
{
	const unsigned char *base = (const unsigned char *)cd + ntohl(cd->hashOffset);
	if (page >= ntohl(cd->nCodeSlots)) {
		return NULL;
	}
	const unsigned char *h = base + (size_t)page * hash_len;
	if ((const char *)h < lower || (const char *)h + hash_len > upper) {
		return NULL;
	}
	return h;
}
static kern_return_t kmem_alloc(int *map, vm_offset_t *addr, vm_size_t size, int f, int t)
{ (void)map; (void)f; (void)t; *addr = (vm_offset_t)calloc(1, size); return *addr ? 0 : 1; }
static void kmem_free(int *map, vm_offset_t addr, vm_size_t size) { (void)map; (void)size; free((void *)addr); }
static kern_return_t vm_map_protect(int *m, vm_offset_t a, vm_offset_t b, int p, int s)
{ (void)m; (void)a; (void)b; (void)p; (void)s; return 0; }
static void *kalloc_data(size_t n, int f) { (void)f; return malloc(n); }
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static int vfs_context_current(void) { return 0; }
static int vfs_context_ucred(int c) { return c; }
static int current_proc(void) { return 0; }
static int vn_rdwr(int rw, vnode_t vp, caddr_t buf, int len, off_t pos, int seg, int flags, int cred,
    int *resid, int proc)
{
	(void)rw; (void)vp; (void)seg; (void)flags; (void)cred; (void)proc;
	reads++;
	ssize_t r = pread(g_fd, buf, (size_t)len, pos);
	if (r < 0) {
		return errno;
	}
	*resid = len - (int)r;
	return 0;
}

#define cs_native4k_prepare old_cs_native4k_prepare
#include "old.c"
#undef cs_native4k_prepare
#define cs_native4k_prepare new_cs_native4k_prepare
#include "new.c"
#undef cs_native4k_prepare

/* Code signature of a thin arm64 Mach-O (LC_CODE_SIGNATURE) or of a dyld cache file. */
static const CS_CodeDirectory *
find_cd(const unsigned char *f, size_t n, const unsigned char **sb_end)
{
	uint32_t off = 0, size = 0;
	if (n > 0x40 && !memcmp(f, "dyld_v1", 7)) {
		uint64_t o, s;
		memcpy(&o, f + 0x28, 8);
		memcpy(&s, f + 0x30, 8);
		off = (uint32_t)o;
		size = (uint32_t)s;
	} else if (n > 32 && *(const uint32_t *)f == 0xfeedfacf) {
		uint32_t ncmds = *(const uint32_t *)(f + 16), p = 32;
		for (uint32_t i = 0; i < ncmds && p + 8 <= n; i++) {
			uint32_t cmd = *(const uint32_t *)(f + p), cs = *(const uint32_t *)(f + p + 4);
			if (cmd == 0x1d) {
				off = *(const uint32_t *)(f + p + 8);
				size = *(const uint32_t *)(f + p + 12);
			}
			p += cs;
		}
	}
	if (!size || (size_t)off + size > n) {
		return NULL;
	}
	const unsigned char *sb = f + off;
	*sb_end = sb + size;
	uint32_t count = ntohl(*(const uint32_t *)(sb + 8));
	const CS_CodeDirectory *best = NULL;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = ntohl(*(const uint32_t *)(sb + 12 + 8 * i));
		uint32_t bo = ntohl(*(const uint32_t *)(sb + 16 + 8 * i));
		const CS_CodeDirectory *cd = (const CS_CodeDirectory *)(sb + bo);
		if ((type == 0 || (type >= 0x1000 && type < 0x1005)) && ntohl(cd->magic) == 0xfade0c02 &&
		    cd->hashType == 2) {
			best = cd;
		}
	}
	return best;
}

static int
run(const char *path, int corrupt)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0) {
		perror(path);
		return 1;
	}
	unsigned char *f = malloc((size_t)st.st_size);
	if (pread(fd, f, (size_t)st.st_size, 0) != st.st_size) {
		perror("read");
		return 1;
	}
	const unsigned char *end = NULL;
	const CS_CodeDirectory *cd = find_cd(f, (size_t)st.st_size, &end);
	if (cd == NULL || cd->pageSize != 14) {
		printf("%-60s skipped (no SHA-256 16K code directory)\n", path);
		return 0;
	}
	char tmp[] = "/tmp/cs4kXXXXXX";
	g_fd = fd;
	if (corrupt) {
		int t = mkstemp(tmp);
		uint32_t at = ntohl(cd->codeLimit) / 2;
		f[at] ^= 0x5a;
		if (write(t, f, (size_t)st.st_size) != st.st_size) {
			return 1;
		}
		f[at] ^= 0x5a;
		g_fd = t;
	}
	struct cs_blob a = { cd, &sha256_hash, 14, 0, 0, (const char *)f, (vm_size_t)(end - f), 0, 0, 0 };
	struct cs_blob b = a;
	unsigned long r0 = reads;
	int ea = old_cs_native4k_prepare(0, &a);
	unsigned long ra = reads - r0;
	r0 = reads;
	int eb = new_cs_native4k_prepare(0, &b);
	unsigned long rb = reads - r0;
	int same = ea == eb && a.csb_native4k_hash_count == b.csb_native4k_hash_count &&
	    (ea != 0 || !memcmp(a.csb_native4k_hashes, b.csb_native4k_hashes,
	    a.csb_native4k_hash_count * 32));
	printf("%-60s %s limit %u: old err %d reads %lu, new err %d reads %lu, %lu 4K digests %s\n",
	    path, corrupt ? "CORRUPT" : "ok     ", ntohl(cd->codeLimit), ea, ra, eb, rb,
	    (unsigned long)a.csb_native4k_hash_count, same ? "IDENTICAL" : "DIFFERENT");
	if (corrupt) {
		unlink(tmp);
		close(g_fd);
	}
	close(fd);
	free(f);
	return !same || (corrupt && (ea == 0 || eb == 0)) || (!corrupt && ea != 0);
}

int
main(int argc, char **argv)
{
	int bad = 0;
	for (int i = 1; i < argc; i++) {
		bad |= run(argv[i], 0);
		bad |= run(argv[i], 1);
	}
	printf("%s\n", bad ? "FAILED" : "all equivalent");
	return bad;
}
