/*
 * PANDORA file installer (patch 0147): writes files from the pinned pack (tools/mkfwpack.py,
 * pandora_fw_read in sweet_ufs.c) into the root file system once it is mounted, through
 * the VFS so the HFS journal stays consistent. The pack's "files.txt" lists
 * "name mode path" lines; name is a file in the pack. A file whose content is already the
 * same is left alone. First user: the default iOS 16 wallpaper's layers with PNG images in
 * place of the HEIC ones (tools/mkwallpaper.py), which need the hardware HEVC decoder.
 * Boot-arg pandora_files=0 turns it off.
 * Patch 0182: files up to 64 MiB, copied and compared 1 MiB at a time (the Turnip library).
 * Patch 0185: a "- MODE PATH" line makes the directory PATH (an existing one counts as the same).
 * Patch 0286: a "> FROM TO" line renames FROM to TO (a missing FROM counts as done).
 * Patch 0297: a "! - PATH" line removes the file PATH (a missing one counts as done).
 * Patch 0308: screen shots on two keys held together (pandora_shot_request, below).
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/mount_internal.h>
#include <sys/kauth.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>

extern kern_return_t thread_terminate(thread_t);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);
extern vnode_t rootvnode;
extern int pandora_mkdir(const char *path, int mode);   /* bsd/vfs/vfs_syscalls.c (patch 0185) */
extern int pandora_rename(const char *from, const char *to);   /* bsd/vfs/vfs_syscalls.c (patch 0286) */
extern int unlink1(vfs_context_t, vnode_t, user_addr_t, enum uio_seg, int);   /* bsd/vfs/vfs_syscalls.c */
void pandora_files_init(void);

#define PF_MAX_FILE   (64u << 20)

struct pf_ent {
	char name[48];
	uint64_t off, size;
};
static struct pf_ent pf_dir[63];
static unsigned pf_n;
static int pf_chan;

static void
pf_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pf_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static struct pf_ent *
pf_find(const char *name)
{
	for (unsigned i = 0; i < pf_n; i++) {
		pf_dir[i].name[47] = 0;
		if (!strcmp(pf_dir[i].name, name)) {
			return &pf_dir[i];
		}
	}
	return NULL;
}

/* Files are copied a chunk at a time (patch 0182: the Turnip library is far bigger than one
 * kernel allocation should be): pack bytes in A, file bytes in B. */
#define PF_CHUNK      (1u << 20)

/* 1 = same content, 0 = differs or missing. */
static int
pf_same(const char *path, const struct pf_ent *e, uint8_t *a, uint8_t *b, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx)) {
		return 0;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	int same = !vnode_getattr(vp, &va, ctx) && va.va_data_size == e->size;
	for (uint64_t at = 0; same && at < e->size; at += PF_CHUNK) {
		int n = (int)MIN((uint64_t)PF_CHUNK, e->size - at), resid = 0;
		same = !pandora_fw_read(e->off + at, a, (uint64_t)n) &&
		    !vn_rdwr(UIO_READ, vp, (caddr_t)b, n, (off_t)at, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx)) && resid == 0 && memcmp(a, b, (size_t)n) == 0;
	}
	vnode_close(vp, FREAD, ctx);
	return same;
}

static int
pf_write(const char *path, const struct pf_ent *e, uint8_t *a, int mode, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, mode, 0, &vp, ctx);
	if (error) {
		return error;
	}
	for (uint64_t at = 0; !error && at < e->size; at += PF_CHUNK) {
		int n = (int)MIN((uint64_t)PF_CHUNK, e->size - at), resid = 0;
		error = pandora_fw_read(e->off + at, a, (uint64_t)n);
		if (!error) {
			error = vn_rdwr(UIO_WRITE, vp, (caddr_t)a, n, (off_t)at, UIO_SYSSPACE, IO_SYNC,
			    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		}
		if (!error && resid) {
			error = EIO;
		}
	}
	int cerr = vnode_close(vp, FWRITE, ctx);
	return error ? error : cerr;
}

static void
pf_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	uint8_t *a = NULL, *b = NULL;
	/* The root file system: wait for it to be mounted. */
	for (int i = 0; i < 600 && rootvnode == NULLVP; i++) {
		pf_sleep(500);
	}
	pf_sleep(2000);
	uint8_t blk[4096];
	uint32_t n = 0;
	if (rootvnode == NULLVP || pandora_fw_read(0, blk, sizeof(blk))) {
		printf("PANDORA FILES: no root or no pack\n");
		goto out;
	}
	memcpy(&n, blk + 8, 4);
	if (memcmp(blk, "PANDFW01", 8) || n == 0 || n > 63) {
		printf("PANDORA FILES: no pack header\n");
		goto out;
	}
	memcpy(pf_dir, blk + 16, n * sizeof(struct pf_ent));
	pf_n = n;
	struct pf_ent *list = pf_find("files.txt");
	if (!list || list->size == 0 || list->size > 16384) {
		printf("PANDORA FILES: no files.txt in the pack\n");
		goto out;
	}
	static char text[16385];
	if (pandora_fw_read(list->off, text, list->size)) {
		goto out;
	}
	text[list->size] = 0;
	vfs_context_t ctx = vfs_context_kernel();
	unsigned written = 0, same = 0, failed = 0;
	a = kalloc_data(PF_CHUNK, Z_WAITOK);
	b = kalloc_data(PF_CHUNK, Z_WAITOK);
	if (!a || !b) {
		printf("PANDORA FILES: no copy buffers\n");
		goto out;
	}
	for (char *line = text, *next; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) {
			*next++ = 0;
		}
		for (size_t len = strlen(line); len && (line[len - 1] == '\r' || line[len - 1] == ' '); len--) {
			line[len - 1] = 0;
		}
		char *sp1 = strchr(line, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
		if (!sp1 || !sp2) {
			continue;
		}
		*sp1 = *sp2 = 0;
		const char *name = line, *path = sp2 + 1;
		int mode = 0;
		for (const char *m = sp1 + 1; *m >= '0' && *m <= '7'; m++) {
			mode = mode * 8 + (*m - '0');
		}
		/* "- MODE PATH": a directory (patch 0185: the Metal plugin is a bundle directory). */
		if (!strcmp(name, "-") && path[0] == '/') {
			int error = pandora_mkdir(path, mode ? mode : 0755);
			if (!error) {
				written++;
				printf("PANDORA FILES: mkdir %s\n", path);
			} else if (error == EEXIST) {
				same++;
			} else {
				failed++;
				printf("PANDORA FILES: mkdir %s: error %d\n", path, error);
			}
			continue;
		}
		/* "> FROM TO" (patch 0286): rename FROM to TO, both absolute paths without spaces; once FROM is gone
		 * there is nothing to do. First user: ios-sweet's root image (v12) moved VirtualAudio.plugin to
		 * /Library/Audio/Plug-Ins/HAL.disabled while the phone had no audio device; it goes back now. */
		if (!strcmp(name, ">") && sp1[1] == '/' && path[0] == '/') {
			const char *from = sp1 + 1;
			int error = pandora_rename(from, path);
			if (!error) {
				written++;
				printf("PANDORA FILES: rename %s -> %s\n", from, path);
			} else if (error == ENOENT) {
				same++;
			} else {
				failed++;
				printf("PANDORA FILES: rename %s -> %s: error %d\n", from, path, error);
			}
			continue;
		}
		/* "! - PATH" (patch 0297): remove the file PATH as the unlink syscall does. First user: the listening
		 * test's player (Camera.app/PP) and its music, gone again after the test as the user asked. */
		if (!strcmp(name, "!") && path[0] == '/') {
			int error = unlink1(ctx, NULLVP, CAST_USER_ADDR_T(path), UIO_SYSSPACE, 0);
			if (!error) {
				written++;
				printf("PANDORA FILES: removed %s\n", path);
			} else if (error == ENOENT) {
				same++;
			} else {
				failed++;
				printf("PANDORA FILES: remove %s: error %d\n", path, error);
			}
			continue;
		}
		struct pf_ent *e = pf_find(name);
		if (!e || e->size > PF_MAX_FILE || path[0] != '/') {
			printf("PANDORA FILES: %s: not in the pack or too big\n", name);
			failed++;
			continue;
		}
		int error = 0;
		if (pf_same(path, e, a, b, ctx)) {
			same++;
		} else if (!(error = pf_write(path, e, a, mode ? mode : 0644, ctx))) {
			written++;
			printf("PANDORA FILES: %s -> %s (%llu bytes)\n", name, path, e->size);
		}
		if (error) {
			failed++;
			printf("PANDORA FILES: %s -> %s: error %d\n", name, path, error);
		}
	}
	printf("PANDORA FILES: %u written, %u already the same, %u failed\n", written, same, failed);
out:
	if (a) {
		kfree_data(a, PF_CHUNK);
	}
	if (b) {
		kfree_data(b, PF_CHUNK);
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * Patch 0308: screen shots. Two of the three keys held together (bsd/dev/fuxi_keys.c; 0309: any two, the iPhone's
 * and Android's screen shot keys included) save what is on screen
 * (iokit/Kernel/arm/SweetFramebuffer.cpp pandora_shot_take: the frame the display pipes were last given, whole, or
 * the panel memory when they read that) as /private/var/tmp/pandora_shot_N.bgra, N counting from 1 each boot: a
 * 64-byte header ("PANDSHOT", version 1, width, height, N, uptime in ms), then the pixels, BGRA, tightly packed.
 * A thread of its own writes them, so the key thread keeps polling. tools/shot_fetch.py makes PNGs of them.
 */
extern void *pandora_shot_take(uint32_t *len, uint32_t *w, uint32_t *h);
extern void pandora_shot_free(void *p, uint32_t len);
void pandora_shot_request(void);

struct ps_head {
	char magic[8];
	uint32_t version, width, height, seq;
	uint64_t uptime_ms;
	uint8_t pad[32];
};
static uint32_t ps_req;
static bool ps_started;

static int
ps_save(const char *path, const struct ps_head *hd, const void *px, uint32_t len)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, 0644, 0, &vp, ctx), resid = 0;
	if (error) {
		return error;
	}
	error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)hd, sizeof(*hd), 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (!error && !resid) {
		error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)px, (int)len, (off_t)sizeof(*hd), UIO_SYSSPACE, IO_SYNC,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	}
	if (!error && resid) {
		error = EIO;
	}
	int cerr = vnode_close(vp, FWRITE, ctx);
	return error ? error : cerr;
}

static void
ps_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	uint32_t seen = 0, seq = 0;
	for (;;) {
		(void)assert_wait(&ps_req, THREAD_UNINT);
		const uint32_t req = __atomic_load_n(&ps_req, __ATOMIC_ACQUIRE);
		if (req == seen) {
			(void)thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		clear_wait(current_thread(), THREAD_AWAKENED);
		seen = req;
		const uint64_t t0 = mach_absolute_time();
		uint32_t len = 0, w = 0, h = 0;
		void *px = pandora_shot_take(&len, &w, &h);
		if (px == NULL || rootvnode == NULLVP) {
			printf("PANDORA SHOT: nothing to save (%s)\n", px == NULL ? "no frame" : "no root");
			if (px != NULL) {
				pandora_shot_free(px, len);
			}
			continue;
		}
		struct ps_head hd;
		bzero(&hd, sizeof(hd));
		memcpy(hd.magic, "PANDSHOT", 8);
		hd.version = 1;
		hd.width = w;
		hd.height = h;
		hd.seq = ++seq;
		uint64_t ns, took;
		absolutetime_to_nanoseconds(t0, &ns);
		hd.uptime_ms = ns / NSEC_PER_MSEC;
		char path[64];
		snprintf(path, sizeof(path), "/private/var/tmp/pandora_shot_%u.bgra", seq);
		const int error = ps_save(path, &hd, px, len);
		pandora_shot_free(px, len);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &took);
		printf("PANDORA SHOT: %u at %llu ms: %s, %ux%u, error %d, %llu ms\n", seq, hd.uptime_ms, path, w, h, error,
		    took / NSEC_PER_MSEC);
	}
}

/* From the key thread: one more shot; the thread starts with the first. */
void
pandora_shot_request(void)
{
	if (!ps_started) {
		thread_t thread;
		if (kernel_thread_start(ps_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("PANDORA SHOT: no thread\n");
			return;
		}
		thread_deallocate(thread);
		ps_started = true;
	}
	__atomic_add_fetch(&ps_req, 1, __ATOMIC_RELEASE);
	thread_wakeup(&ps_req);
}

void
pandora_files_init(void)
{
	thread_t thread;
	int on = 1;
	(void)PE_parse_boot_argn("pandora_files", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(pf_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
