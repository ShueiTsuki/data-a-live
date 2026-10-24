/*
 * pandora-loader: act as iBoot for the PANDORA xnu on the Xiaomi 17 Pro (SM8850).
 * Started by `fastboot boot`; writes nothing to storage.
 *
 * The payload (tools/mkpayload.py from ios-sweet: kernel segments with chained
 * fixups already resolved, ADT, optional RAM disk) is embedded in this image.
 * Like ios-sweet's shim/loader.c: copy segments to phys_base + (vmaddr - virt_base),
 * place the ADT and boot_args behind the kernel, jump to the entry with the MMU and
 * caches off and x0 = boot_args (physical). Framebuffer values were measured on this
 * phone by shim/hello (0xfc800000, 1220x2656, stride 4880, ARGB8888, self-refreshing).
 *
 * MMU is off: every access is Device memory (-mstrict-align, 8-byte copies).
 * Linked at 0 and loaded anywhere: no absolute pointers in data.
 *
 * v2: like iBoot, without "-v" among the payload's boot-args the screen shows only the boot
 * logo (logo.S, tools/bootlogo.py) centred on black, and the kernel keeps it there (patch 0206)
 * until iOS presents its first frame. With "-v", or when something goes wrong, text as before.
 */
#include <stdint.h>
#include "font.h"

void fatal(void) __attribute__((noreturn));

#define FB          0xfc800000ull
#define FB_SIZE     0x2b00000ull
#define FB_W        1220u
#define FB_H        2656u
#define FB_STRIDE   (1220u * 4)
/* Kernel RAM must stay inside the clean 0x8c0000000 + 13 GiB range (DT_FINDINGS.md). */
#define RAM_LO      0x8c0000000ull
#define RAM_HI      0xc00000000ull
#define BOOT_LINE_LENGTH 608
#define PAGE_16K    0x4000ull

/* ---- console (from shim/hello) ---- */
#define SCALE  3
#define CW     (8 * SCALE)
#define CH     (16 * SCALE)
#define MARGIN 24
#define BG     0xff102040u
#define WHITE  0xffffffffu
#define YELLOW 0xffffff00u
#define GREEN  0xff40ff40u
#define RED    0xffff4040u
#define BLACK  0xff000000u

static volatile uint32_t *const fb = (volatile uint32_t *)(uintptr_t)FB;
static int row;
static int verbose = 1;     /* text on screen; 0 = the boot logo only */

static void fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h && y < FB_H; y++) {
		volatile uint32_t *line = fb + y * (FB_STRIDE / 4);
		for (uint32_t x = x0; x < x0 + w && x < FB_W; x++)
			line[x] = c;
	}
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CW, y0 = MARGIN + row * CH;
	if (x0 + CW > FB_W || y0 + CH > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			    (g[gy] & (0x80 >> gx)) ? fg : BG);
}

static void line(const char *label, const char *text, uint32_t fg)
{
	if (!verbose)
		return;
	int c = 0;
	while (*label)
		put_char(c++, *label++, YELLOW);
	c = 10;
	while (*text)
		put_char(c++, *text++, fg);
	row++;
}

static void hexline(const char *label, uint64_t v)
{
	char b[19];
	b[0] = '0'; b[1] = 'x';
	for (int i = 0; i < 16; i++) {
		unsigned d = (v >> (4 * (15 - i))) & 0xf;
		b[2 + i] = d < 10 ? '0' + d : 'A' + d - 10;
	}
	b[18] = 0;
	line(label, b, WHITE);
}

/* ---- payload (ios-sweet tools/mkpayload.py layout) ---- */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

extern const unsigned char _payload_start[], _payload_end[];
extern const unsigned char _logo_start[], _logo_end[];

/* "-v" as a word among the embedded payload's boot-args; also yes when there is no payload,
 * so that the error is shown. */
static int wants_verbose(void)
{
	const struct payload *p = (const struct payload *)(uintptr_t)_payload_start;
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (p->magic[i] != want[i])
			return 1;
	const char *c = p->cmdline;
	for (int i = 0; i + 1 < BOOT_LINE_LENGTH && c[i]; i++)
		if (c[i] == '-' && c[i + 1] == 'v' && (i == 0 || c[i - 1] == ' ') &&
		    (i + 2 == BOOT_LINE_LENGTH || c[i + 2] == ' ' || c[i + 2] == 0))
			return 1;
	return 0;
}

/* The logo (tools/bootlogo.py: "PLOGO\0\0\1", width, height, ARGB pixels) centred, as iBoot
 * draws its applelogo. Nothing when logo.S is empty. */
static void draw_logo(void)
{
	const volatile uint32_t *l = (const volatile uint32_t *)(uintptr_t)_logo_start;
	uint64_t n = (uintptr_t)_logo_end - (uintptr_t)_logo_start;
	static const char magic[8] = { 'P', 'L', 'O', 'G', 'O', 0, 0, 1 };
	const volatile unsigned char *m = (const volatile unsigned char *)l;
	if (n < 16)
		return;
	for (int i = 0; i < 8; i++)
		if (m[i] != (unsigned char)magic[i])
			return;
	uint32_t w = l[2], h = l[3];
	if (w > FB_W || h > FB_H || n < 16 + (uint64_t)w * h * 4)
		return;
	uint32_t x0 = (FB_W - w) / 2, y0 = (FB_H - h) / 2;
	for (uint32_t y = 0; y < h; y++) {
		volatile uint32_t *dst = fb + (y0 + y) * (FB_STRIDE / 4) + x0;
		const volatile uint32_t *src = l + 4 + (uint64_t)y * w;
		for (uint32_t x = 0; x < w; x++)
			dst[x] = src[x];
	}
}

static void copy64(uint64_t dst, uint64_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)(uintptr_t)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uint64_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int in_ram(uint64_t a, uint64_t n)
{
	return a >= RAM_LO && n <= RAM_HI - a;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static const char *boot(void)
{
	uint64_t pl = (uintptr_t)_payload_start, pl_end = (uintptr_t)_payload_end;
	const struct payload *p = (const struct payload *)(uintptr_t)pl;
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (p->magic[i] != want[i])
			return "no SWEETXNU payload embedded";
	if (p->version != 1 || (p->adt_off & 7) || (pl & 7) || p->nsegs == 0 || p->nsegs > 64)
		return "bad payload header";
	hexline("PAYLOAD", pl);
	hexline("PHYS", p->phys_base);
	hexline("MEMSIZE", p->mem_size);
	hexline("ENTRY", p->entry_va);
	if (!in_ram(p->phys_base, p->mem_size))
		return "kernel RAM outside the clean 0x8c0000000 range";
	if (pl_end > p->phys_base && pl < p->phys_base + p->mem_size)
		return "payload overlaps kernel memory";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;
	/* Check every destination before writing anything. */
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		if (!in_ram(s->vmaddr + pa_of_va_base, s->vmsize) || s->file_size > s->vmsize ||
		    s->file_off + s->file_size > pl_end - pl)
			return "unsafe payload segment";
	}
	uint64_t adt_pa = p->kernel_end + pa_of_va_base;
	uint64_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uint64_t top = ba_pa + PAGE_16K;
	if (!in_ram(adt_pa, top - adt_pa) || top > p->phys_base + p->mem_size ||
	    p->adt_off + p->adt_size > pl_end - pl)
		return "ADT/boot_args outside kernel RAM";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uint64_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, pl + s->file_off, s->file_size);
		uint64_t done = (s->file_size + 7) & ~7ull;
		if (s->vmsize > done)
			zero64(dst + done, s->vmsize - done);
	}
	hexline("KERN_END", p->kernel_end + pa_of_va_base);

	copy64(adt_pa, pl + p->adt_off, p->adt_size);
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)(uintptr_t)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* read with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	hexline("BOOTARGS", ba_pa);

	uint64_t entry_pa = p->entry_va + pa_of_va_base;
	if (!in_ram(entry_pa, 4))
		return "entry outside kernel RAM";
	hexline("JUMP", entry_pa);
	line("XNU", "jumping (the kernel console takes over)", GREEN);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)(uintptr_t)entry_pa)(ba_pa);
	return "xnu returned";
}

void loader_main(uint64_t fdt, uint64_t el, uint64_t load)
{
	(void)fdt;
	row = 0;
	verbose = wants_verbose();
	if (verbose) {
		fill(0, 0, FB_W, FB_H, BG);
	} else {
		fill(0, 0, FB_W, FB_H, BLACK);
		draw_logo();
	}
	line("PANDORA", "loader v2 - nothing is written to storage", GREEN);
	line("LEVEL", el == 1 ? "EL1" : "NOT EL1", el == 1 ? WHITE : RED);
	hexline("LOADED", load);
	const char *err = el == 1 ? boot() : "XNU needs EL1";
	if (!verbose) {     /* an error is always shown, as text */
		verbose = 1;
		fill(0, 0, FB_W, FB_H, BG);
		line("PANDORA", "loader v2 - nothing is written to storage", GREEN);
		hexline("LOADED", load);
	}
	line("ERROR", err, RED);
	/* Leave the message up for 60 s, then go back to Android. */
	uint64_t f, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
	do
		__asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
	while (t - t0 < 60 * f);
	fatal();
}
