/*
 * pandora boot menu: lives in boot_b in place of the stock Android kernel, so the phone can
 * start iOS or Android at power-on without a PC. Written once; iOS is changed by replacing
 * only the iOS slot (update_ios.sh).
 *
 * ABL loads all of boot_b's kernel field as it loads the stock kernel: vendor_boot,
 * init_boot (Magisk), dtbo and the bootconfig still come from flash, and x0 is ABL's FDT.
 * The field holds this code, the settings page, the stock Android kernel (2 MiB in) and the
 * iOS slot: a header page and an arm64 Image, the kernel of what `fastboot boot` starts
 * (shim/loader with its XNU payload). This code does not look inside that Image.
 *
 * On a normal power-on one line is drawn near the bottom of the boot logo for a few seconds.
 * Volume up: the slot is checked (marker, armed, size, checksum) and its Image is entered
 * exactly as ABL would enter it (x0 = FDT, MMU off). Otherwise the logo is put back and the
 * stock Android kernel is entered with the same FDT, as the stock boot_b would. Charger,
 * silent and every other boot mode go to Android at once. Writes nothing to storage.
 *
 * MMU is off: every access is Device memory (-mstrict-align, byte or aligned reads).
 * Linked at 0 and loaded anywhere: no absolute pointers in data.
 */
#include <stdint.h>
#include "font.h"

void fatal(void) __attribute__((noreturn));
void jump_linux(uint64_t entry, uint64_t fdt) __attribute__((noreturn));
uint32_t safe_rd32(uint64_t pa, uint32_t *faulted);

#define FB          0xfc800000ull
#define FB_W        1220u
#define FB_H        2656u
#define FB_STRIDE   (1220u * 4)
#define SZ_2M       0x200000ull
#define SLOT_HDR    0x1000ull
/* Volume up: TLMM GPIO 101, active low (xnu patches 0128/0130; 0x0f100000 + 0x1000 * n,
 * the DT's 0x0f065000 hangs the bus). +0 config (FUNC bits 2-5, OE bit 9), +4 bit 0 input. */
#define VOLUP_GPIO  0x0f165000ull

/* ---- settings page, filled in by mkboot.py ---- */
struct bm_config {
	char magic[8];			/* "PNDBMCF2" */
	uint32_t wait_ms;		/* how long the line stays up */
	uint32_t any_mode;		/* test builds: the line in every boot mode, and the numbers */
	uint64_t slot_off;		/* iOS slot, from the start of this Image */
	uint64_t slot_max;		/* its size, header page included */
	uint64_t save_off;		/* 1 MiB for the screen band, inside image_size */
};

__attribute__((section(".config"), used, aligned(4096)))
volatile struct bm_config bm_config = { .magic = { 'P', 'N', 'D', 'B', 'M', 'C', 'F', '2' } };

/* ---- iOS slot header page, written by update_ios.sh (slot.py) ---- */
struct bm_slot {
	char magic[8];			/* "PNDSLOT1" */
	uint32_t armed;			/* 1: may start; written last by update_ios.sh, cleared by the guard */
	uint32_t reserved;
	uint64_t size;			/* bytes of the Image after this page, a multiple of 8 */
	uint64_t sum;			/* bm_sum() of those bytes */
	uint8_t extents_sha256[3][32];	/* iosroot, ioslog, adspfw: pinfile `extents --json` */
	char why[64];			/* set by the guard with armed = 0 */
	char label[64];			/* which build (update_ios.sh), not shown */
};

_Static_assert(__builtin_offsetof(struct bm_config, slot_off) == 16, "settings layout");
_Static_assert(__builtin_offsetof(struct bm_slot, sum) == 24, "slot layout");
_Static_assert(__builtin_offsetof(struct bm_slot, why) == 128, "slot layout");

extern const unsigned char _linux_start[], _linux_end[];

static uint8_t rd8(uint64_t a) { return *(volatile const uint8_t *)(uintptr_t)a; }
static uint32_t rd32(uint64_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint64_t rd64(uint64_t a) { return *(volatile const uint64_t *)(uintptr_t)a; }

static uint32_t be32(uint64_t a)
{
	return (uint32_t)rd8(a) << 24 | (uint32_t)rd8(a + 1) << 16 | (uint32_t)rd8(a + 2) << 8 | rd8(a + 3);
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static uint64_t tick_hz(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}

static void wait_ms(uint64_t ms)
{
	uint64_t t0 = ticks(), n = tick_hz() * ms / 1000;
	while (ticks() - t0 < n)
		;
}

/* ---- screen (from shim/loader) ---- */
#define SCALE  3
#define CW     (8 * SCALE)
#define CH     (16 * SCALE)
#define MARGIN 24
#define BG     0xff102040u
#define BLACK  0xff000000u
#define WHITE  0xffffffffu
#define GREY   0xff909090u
#define YELLOW 0xffffff00u
#define RED    0xffff4040u
/* The menu line: a band near the bottom of the logo, saved first and put back after. */
#define BAND_Y (FB_H - 400u)
#define BAND_H (CH + 48u)
#define BAND_BYTES ((uint64_t)FB_STRIDE * BAND_H)

static volatile uint32_t *const fb = (volatile uint32_t *)(uintptr_t)FB;
static int row;

static void fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h && y < FB_H; y++) {
		volatile uint32_t *line = fb + y * (FB_STRIDE / 4);
		for (uint32_t x = x0; x < x0 + w && x < FB_W; x++)
			line[x] = c;
	}
}

static void glyph(uint32_t x0, uint32_t y0, char ch, uint32_t fg, uint32_t bg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	if (x0 + CW > FB_W || y0 + CH > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			    (g[gy] & (0x80 >> gx)) ? fg : bg);
}

static uint32_t slen(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}

static uint32_t scopy(char *d, uint32_t at, const char *s)
{
	while (*s)
		d[at++] = *s++;
	d[at] = 0;
	return at;
}

static void line(const char *label, const char *text, uint32_t fg)
{
	uint32_t y = MARGIN + row * CH, c = 0;
	while (*label)
		glyph(MARGIN + CW * c++, y, *label++, YELLOW, BG);
	c = 10;
	while (*text)
		glyph(MARGIN + CW * c++, y, *text++, fg, BG);
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

/* One centred line in the band, black behind it. */
static void band_text(const char *s, uint32_t fg)
{
	fill(0, BAND_Y, FB_W, BAND_H, BLACK);
	uint32_t n = slen(s), x = n * CW < FB_W ? (FB_W - n * CW) / 2 : 0;
	for (uint32_t i = 0; i < n; i++)
		glyph(x + i * CW, BAND_Y + 24, s[i], fg, BLACK);
}

static void band_copy(uint64_t save, int restore)
{
	volatile uint64_t *f = (volatile uint64_t *)(uintptr_t)(FB + (uint64_t)BAND_Y * FB_STRIDE);
	volatile uint64_t *s = (volatile uint64_t *)(uintptr_t)save;
	for (uint64_t i = 0; i < BAND_BYTES / 8; i++) {
		if (restore)
			f[i] = s[i];
		else
			s[i] = f[i];
	}
}

/* ---- ABL's FDT: /chosen properties ---- */
static int name_is(uint64_t a, const char *s)
{
	while (*s)
		if (rd8(a++) != (uint8_t)*s++)
			return 0;
	return rd8(a) == 0;
}

/* Address of /chosen/<prop>'s value and its length, or 0. */
static uint64_t chosen_prop(uint64_t fdt, const char *prop, uint32_t *len)
{
	if (fdt == 0 || (fdt & 3) || be32(fdt) != 0xd00dfeedu)
		return 0;
	uint32_t total = be32(fdt + 4), off_struct = be32(fdt + 8), off_strings = be32(fdt + 12);
	uint32_t size_struct = be32(fdt + 36);
	if (total > (16u << 20) || off_struct >= total || size_struct > total - off_struct ||
	    off_strings >= total)
		return 0;
	uint64_t p = fdt + off_struct, end = p + size_struct;
	int depth = 0, in_chosen = 0;
	while (p + 4 <= end) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {			/* FDT_BEGIN_NODE */
			depth++;
			if (depth == 2)
				in_chosen = name_is(p, "chosen");
			while (p < end && rd8(p))
				p++;
			p = (p + 4) & ~3ull;
		} else if (tok == 2) {		/* FDT_END_NODE */
			if (depth == 2)
				in_chosen = 0;
			depth--;
		} else if (tok == 3) {		/* FDT_PROP */
			uint32_t l = be32(p), nameoff = be32(p + 4);
			p += 8;
			if (l > end - p)
				return 0;
			if (in_chosen && depth == 2 && off_strings + nameoff < total &&
			    name_is(fdt + off_strings + nameoff, prop)) {
				*len = l;
				return p;
			}
			p = (p + l + 3) & ~3ull;
		} else if (tok != 4) {		/* FDT_NOP; FDT_END or garbage ends the walk */
			break;
		}
	}
	return 0;
}

/* `w` is one whole space-separated word of the string at s (n bytes). */
static int has_word(uint64_t s, uint32_t n, const char *w)
{
	uint32_t wl = slen(w);
	for (uint32_t i = 0; i + wl <= n; i++) {
		if (i && rd8(s + i - 1) != ' ')
			continue;
		uint32_t k = 0;
		while (k < wl && rd8(s + i + k) == (uint8_t)w[k])
			k++;
		if (k == wl && (i + wl == n || rd8(s + i + wl) == ' ' || rd8(s + i + wl) == 0))
			return 1;
	}
	return 0;
}

/* A plain power-on or reboot: the line is shown only then (the words ABL writes on this phone,
 * device/pandora-live.dts). Anything else, charger mode included, goes straight to Android. */
static int normal_mode(uint64_t fdt)
{
	uint32_t n = 0;
	uint64_t args = chosen_prop(fdt, "bootargs", &n);
	return args && has_word(args, n, "bootmonitor.bootmode=normal") &&
	    has_word(args, n, "mca_log.charge_boot_mode=0") &&
	    has_word(args, n, "silent_boot.mode=nonsilent");
}

static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
	return a < b + bn && b < a + an;
}

/* ABL put the FDT and the initrd (ramdisks + bootconfig) outside the memory this image
 * declared, so the save area and an alignment move cannot touch them. */
static int layout_ok(uint64_t fdt, uint64_t load, uint64_t declared)
{
	if (fdt == 0 || be32(fdt) != 0xd00dfeedu || overlaps(fdt, be32(fdt + 4), load, declared))
		return 0;
	uint32_t ls = 0, le = 0;
	uint64_t s = chosen_prop(fdt, "linux,initrd-start", &ls);
	uint64_t e = chosen_prop(fdt, "linux,initrd-end", &le);
	if (!s || !e || ls != 8 || le != 8)
		return 0;
	uint64_t is = (uint64_t)be32(s) << 32 | be32(s + 4), ie = (uint64_t)be32(e) << 32 | be32(e + 4);
	return ie >= is && !overlaps(is, ie - is, load, declared);
}

/* ---- slot checksum ---- */
/* Over 8-byte little-endian words: a += w, b += a (mod 2^64); a ^ (b rotated by 32).
 * slot.py computes the same. Word reads only: fast enough uncached. */
static uint64_t bm_sum(uint64_t p, uint64_t n)
{
	volatile const uint64_t *w = (volatile const uint64_t *)(uintptr_t)p;
	uint64_t a = 0, b = 0;
	for (uint64_t i = 0; i < n / 8; i++) {
		a += w[i];
		b += a;
	}
	return a ^ (b << 32 | b >> 32);
}

/* ---- the iOS slot ---- */
static int magic_is(uint64_t a, const char *m)
{
	for (int i = 0; i < 8; i++)
		if (rd8(a + i) != (uint8_t)m[i])
			return 0;
	return 1;
}

/* 0 when the slot may be started (without the checksum), else why not. */
static const char *slot_state(uint64_t slot, uint64_t max)
{
	volatile const struct bm_slot *h = (volatile const struct bm_slot *)(uintptr_t)slot;
	if (max < SLOT_HDR || !magic_is(slot, "PNDSLOT1"))
		return "no iOS in the slot";
	if (h->armed != 1)
		return 0;		/* not armed: the caller shows h->why */
	uint64_t n = h->size;
	if (n == 0 || (n & 7) || n > max - SLOT_HDR || rd32(slot + SLOT_HDR + 56) != 0x644d5241u)
		return "slot size or image bad";
	return 0;
}

static void slot_why(uint64_t slot, char *msg, uint32_t n)
{
	volatile const struct bm_slot *h = (volatile const struct bm_slot *)(uintptr_t)slot;
	for (uint32_t i = 0; i < 40 && h->why[i] >= 0x20 && h->why[i] < 0x7f; i++)
		msg[n++] = h->why[i];
	msg[n] = 0;
}

/* ---- menu ---- */
static int volup_usable(void)
{
	uint32_t faulted = 0, cfg = safe_rd32(VOLUP_GPIO, &faulted);
	return !faulted && (cfg & 0x3cu) == 0 && !(cfg & 0x200u);
}

static int volup_pressed(void)
{
	uint32_t faulted = 0, io = safe_rd32(VOLUP_GPIO + 4, &faulted);
	return !faulted && !(io & 1u);
}

/* Shows "iOS off: <why>" for the wait (0), or the countdown until volume up (1) or the
 * time is up (0). The band is put back unless iOS is chosen. */
static int menu(uint64_t save, uint64_t slot, uint64_t max)
{
	band_copy(save, 0);
	/* Built by hand: no string initialisers (they become memcpy calls). */
	char msg[96];
	const char *bad = slot_state(slot, max);
	volatile const struct bm_slot *h = (volatile const struct bm_slot *)(uintptr_t)slot;
	if (bad || h->armed != 1) {
		uint32_t n = scopy(msg, 0, "iOS off: ");
		if (bad)
			scopy(msg, n, bad);
		else
			slot_why(slot, msg, n);
		band_text(msg, GREY);
		wait_ms(bm_config.wait_ms);
		band_copy(save, 1);
		return 0;
	}
	uint32_t n = scopy(msg, 0, "Volume up  iOS   ");
	msg[n + 1] = 0;
	uint64_t hz = tick_hz(), t0 = ticks(), total = hz * bm_config.wait_ms / 1000;
	uint32_t held = 0, shown = ~0u;
	for (;;) {
		uint64_t t = ticks() - t0;
		if (t >= total)
			break;
		uint32_t left = (uint32_t)((total - t + hz - 1) / hz);
		if (left != shown) {
			msg[n] = left > 9 ? '9' : '0' + left;
			band_text(msg, WHITE);
			shown = left;
		}
		held = volup_pressed() ? held + 1 : 0;
		if (held >= 3) {		/* 30 ms */
			if (bm_sum(slot + SLOT_HDR, h->size) == h->sum)
				return 1;
			band_text("iOS off: slot damaged", GREY);
			wait_ms(3000);
			break;
		}
		wait_ms(10);
	}
	band_copy(save, 1);
	return 0;
}

/* ---- Android: the stock kernel, unchanged, with ABL's FDT ---- */
static void halt_with(const char *label, const char *err) __attribute__((noreturn));
static void halt_with(const char *label, const char *err)
{
	line(label, err, RED);
	/* Leave the message up for 60 s, then reset. */
	wait_ms(60000);
	fatal();
}

static void start_android(uint64_t fdt, uint64_t load, uint64_t declared, int layout)
{
	uint64_t src = (uintptr_t)_linux_start, n = (uintptr_t)_linux_end - src;
	if (rd32(src + 56) != 0x644d5241u)	/* "ARM\x64" */
		halt_with("ANDROID", "no arm64 kernel embedded");
	uint64_t text_offset = rd64(src + 8), size = rd64(src + 16);
	/* arm64 boot protocol: text_offset bytes above a 2 MiB aligned base. */
	uint64_t dst = ((src - text_offset + SZ_2M - 1) & ~(SZ_2M - 1)) + text_offset;
	if (dst != src) {
		if (!layout || dst + size > load + declared)
			halt_with("ANDROID", "kernel not 2 MiB aligned and no room to move it");
		/* Upwards, overlapping: copy from the end. (Only the slot behind it is lost.) */
		volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
		volatile const uint64_t *s = (volatile const uint64_t *)(uintptr_t)src;
		for (uint64_t i = (n + 7) / 8; i-- > 0;)
			d[i] = s[i];
	}
	jump_linux(dst, fdt);
}

void menu_main(uint64_t fdt, uint64_t el, uint64_t load)
{
	uint64_t declared = rd64(load + 16);
	uint64_t slot = load + bm_config.slot_off, max = bm_config.slot_max;
	uint64_t save = load + bm_config.save_off;
	int layout = layout_ok(fdt, load, declared);
	int room = layout && bm_config.save_off + BAND_BYTES <= declared &&
	    bm_config.slot_off + max <= declared && (slot & 0xfff) == 0;
	if (bm_config.any_mode) {
		/* Test builds: what the menu decides from, at the top of the screen (Android
		 * draws over it later). */
		uint32_t ls = 0, le = 0, faulted = 0, cfg = safe_rd32(VOLUP_GPIO, &faulted);
		uint64_t s = chosen_prop(fdt, "linux,initrd-start", &ls);
		uint64_t e = chosen_prop(fdt, "linux,initrd-end", &le);
		const char *st = slot_state(slot, max);
		row = 0;
		hexline("LOADED", load);
		hexline("IMGSIZE", declared);
		hexline("FDT", fdt);
		hexline("FDTSIZE", be32(fdt) == 0xd00dfeedu ? be32(fdt + 4) : 0);
		hexline("INITRD", s && ls == 8 ? (uint64_t)be32(s) << 32 | be32(s + 4) : 0);
		hexline("INITRDEND", e && le == 8 ? (uint64_t)be32(e) << 32 | be32(e + 4) : 0);
		hexline("SLOT", slot);
		hexline("SLOTSIZE", rd64(slot + 16));
		line("SLOTSTATE", st ? st : rd32(slot + 8) == 1 ? "armed" : "off", WHITE);
		hexline("STATE", el << 32 | (uint64_t)faulted << 28 | (uint64_t)(cfg & 0xfffffff));
		hexline("CHECKS", (uint64_t)layout << 12 | (uint64_t)room << 8 | (uint64_t)normal_mode(fdt) << 4 |
		    (uint64_t)volup_usable());
	}
	if (room && el == 1 && (bm_config.any_mode || normal_mode(fdt)) && volup_usable() &&
	    menu(save, slot, max)) {
		/* The slot's Image, entered as ABL enters a kernel. It draws its own screen. */
		jump_linux(slot + SLOT_HDR, fdt);
	}
	if (bm_config.any_mode)
		wait_ms(bm_config.wait_ms);	/* time to read the lines above */
	start_android(fdt, load, declared, layout);
}
