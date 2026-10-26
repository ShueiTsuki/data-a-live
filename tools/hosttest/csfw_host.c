/*
 * Host tests for wsl/pandora_csfw.c (Cirrus Halo DSP files as register writes).
 *
 *   csfw_host                       built-in cases only (no vendor files needed)
 *   csfw_host DIR [REGDUMP]         also the phone's files in DIR (cs35l43-dsp1-spk-prot.wmfw and
 *                                   the T-/B- .bin files); REGDUMP: an amplifier's regmap dump
 *                                   ("addr: value" lines) whose XM id header must equal the decoded one
 *
 * The vendor files and dumps stay outside the repository.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../wsl/pandora_csfw.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Four 24-bit words into three big-endian registers, word 0 in the low bits of the first. */
static void
pack4(uint8_t *b, const uint32_t w[4])
{
	const uint32_t r0 = (w[0] & 0xffffffu) | w[1] << 24;
	const uint32_t r1 = (w[1] >> 8 & 0xffffu) | w[2] << 16;
	const uint32_t r2 = (w[2] >> 16 & 0xffu) | (w[3] & 0xffffffu) << 8;
	put_be32(b, r0);
	put_be32(b + 4, r1);
	put_be32(b + 8, r2);
}

static uint32_t
region(uint8_t *f, uint32_t at, uint32_t type, uint32_t offset, const uint8_t *data, uint32_t n)
{
	put_le32(f + at, offset | type << 24);
	put_le32(f + at + 4, n);
	memcpy(f + at + 8, data, n);
	return at + 8 + n;
}

/* A length-prefixed string (LB length bytes), padded to 4 bytes with its length. */
static uint32_t
put_text(uint8_t *b, uint32_t at, unsigned lb, const char *s)
{
	const uint32_t n = (uint32_t)strlen(s);
	b[at] = (uint8_t)n;
	if (lb == 2) {
		b[at + 1] = (uint8_t)(n >> 8);
	}
	memcpy(b + at + lb, s, n);
	return at + ((n + lb + 3u) & ~3u);
}

/* One control of an algorithm description. */
static uint32_t
put_ctl(uint8_t *b, uint32_t at, uint32_t offset, uint32_t type, const char *name, uint32_t len)
{
	uint32_t p = put_text(b, at + 8, 1, name);
	p = put_text(b, p, 1, "FULL_NAME_OF_IT");
	p = put_text(b, p, 2, "a description");
	put_le32(b + p, 0x1000);          /* control type, flags */
	put_le32(b + p + 4, len);
	p += 8;
	put_le32(b + at, offset | type << 16);
	put_le32(b + at + 4, p - at - 8);
	return p;
}

/* Algorithm descriptions: controls found by name at their algorithm's base (and the firmware's own). */
static void
controls(void)
{
	uint32_t words[24] = {0x40000, 0x30000, 2, 0x1234, 0x10203, 0x10, 0x20, 0x30, 0x40, 2,
	                      0x5f200, 0x10100, 0x100, 0x8, 0, 0, 0x5f201, 0x20000, 0x200, 0x4, 0x40, 0x2,
	                      0xbedead, 0};
	uint8_t xm[72], f[1024], d[256];
	for (unsigned i = 0; i < 6; i++) {
		pack4(xm + 12 * i, words + 4 * i);
	}
	memset(f, 0, sizeof(f));
	memcpy(f, "WMFW", 4);
	put_le32(f + 4, 40);
	f[10] = 4;
	f[11] = 3;
	uint32_t at = region(f, 40, CSFW_XM_PACKED_T, 0, xm, sizeof(xm));
	memset(d, 0, sizeof(d));
	put_le32(d, 0x5f201);
	uint32_t q = put_text(d, 4, 1, "ALG");
	q = put_text(d, q, 2, "second algorithm");
	put_le32(d + q, 2);
	q = put_ctl(d, q + 4, 3, CSFW_XM_T, "X", 4);
	q = put_ctl(d, q, 5, CSFW_YM_T, "YY", 8);
	at = region(f, at, CSFW_ALGDATA_T, 0, d, q);
	memset(d, 0, sizeof(d));
	put_le32(d, 0x1234);
	q = put_text(d, 4, 1, "FW");
	q = put_text(d, q, 2, "");
	put_le32(d + q, 1);
	q = put_ctl(d, q + 4, 1, CSFW_XM_T, "HB", 4);
	const uint32_t last = at;
	at = region(f, at, CSFW_ALGDATA_T, 0, d, q);
	CsfwInfo info;
	CsfwCtl c;
	CHECK(csfw_wmfw_info(f, at, &info) == CsfwOk);
	CHECK(csfw_find_ctl(f, at, &info, 0x5f201, "X", &c) == CsfwOk && c.type == CSFW_XM_T && c.offset == 3 &&
	    c.bytes == 4 && c.reg == CSFW_XM + 4 * (0x200 + 3));
	CHECK(csfw_find_ctl(f, at, &info, 0x5f201, "YY", &c) == CsfwOk && c.type == CSFW_YM_T &&
	    c.reg == CSFW_YM + 4 * (0x40 + 5) && c.bytes == 8);
	CHECK(csfw_find_ctl(f, at, &info, 0x1234, "HB", &c) == CsfwOk && c.reg == CSFW_XM + 4 * (0x10 + 1));
	CHECK(csfw_find_ctl(f, at, &info, 0x5f201, "Y", &c) == CsfwNoAlg);
	CHECK(csfw_find_ctl(f, at, &info, 0x5f200, "X", &c) == CsfwNoAlg);
	CHECK(csfw_find_ctl(f, at, &info, 0x5f202, "X", &c) == CsfwNoAlg);
	/* the firmware's own description cut inside its control */
	put_le32(f + last + 4, q - 6);
	CHECK(csfw_find_ctl(f, last + 8 + q - 6, &info, 0x1234, "HB", &c) == CsfwShort);
}

static void
builtin(void)
{
	uint32_t w[4] = {0x123456, 0xabcdef, 0x000001, 0xfedcba}, back[4];
	uint8_t b[12];
	pack4(b, w);
	csfw_unpack(b, back);
	CHECK(memcmp(w, back, sizeof(w)) == 0);
	CHECK(csfw_region_reg(CSFW_XM_PACKED_T, 0x479) == ((0x02000000u + 0x479u * 3u) & ~3u));
	CHECK(csfw_region_reg(CSFW_PM_PACKED_T, 2) == 0x0380000au);
	CHECK(csfw_region_reg(CSFW_YM_T, 0x10) == 0x03400040u);

	/* A firmware: id header with two algorithms, a text, an algorithm description, PM and YM. */
	uint32_t words[24] = {0x40000, 0x30000, 2, 0x1234, 0x10203, 0x10, 0x20, 0x30, 0x40, 2,
	                      0x5f200, 0x10100, 0x100, 0x8, 0, 0, 0x5f201, 0x20000, 0x200, 0x4, 0x40, 0x2,
	                      0xbedead, 0};
	uint8_t xm[72], f[512];
	for (unsigned i = 0; i < 6; i++) {
		pack4(xm + 12 * i, words + 4 * i);
	}
	memset(f, 0, sizeof(f));
	memcpy(f, "WMFW", 4);
	put_le32(f + 4, 40);
	f[8] = 0;
	f[9] = 3;
	f[10] = 4;
	f[11] = 3;
	uint32_t at = 40;
	static const uint8_t text[8] = "built-in";
	uint8_t pm[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, ym[8] = {0, 0, 0, 1, 0, 0, 0, 2}, pad[12] = {0};
	at = region(f, at, CSFW_INFO_T, 0, text, 8);
	at = region(f, at, CSFW_XM_PACKED_T, 0, xm, sizeof(xm));
	at = region(f, at, CSFW_ALGDATA_T, 0, pad, 12);
	at = region(f, at, CSFW_PM_PACKED_T, 4, pm, 8);   /* 8: whole registers */
	at = region(f, at, CSFW_YM_T, 0x30, ym, 8);
	CsfwInfo info;
	CHECK(csfw_wmfw_info(f, at, &info) == CsfwOk);
	CHECK(info.vendor == 2 && info.fw_id == 0x1234 && info.n_algs == 2);
	CHECK(info.algs[1].id == 0x5f201 && info.algs[1].xm_base == 0x200 && info.algs[1].ym_base == 0x40);
	CsfwIter it;
	CsfwWrite wr;
	CHECK(csfw_wmfw_begin(&it, f, at) == CsfwOk);
	unsigned n = 0;
	uint32_t regs[4];
	while (csfw_next(&it, 0, &wr) == CsfwOk && n < 4) {
		regs[n++] = wr.reg;
	}
	CHECK(n == 3);
	CHECK(regs[0] == CSFW_XM_PACKED && regs[1] == CSFW_PM_PACKED + 20 && regs[2] == CSFW_YM + 0xc0);
	f[11] = 2;
	CHECK(csfw_wmfw_begin(&it, f, at) == CsfwBadHeader);

	/* Coefficients: a name, a block for algorithm 0x5f201 in XM, one in YM, an absolute register. */
	uint8_t c[256];
	memset(c, 0, sizeof(c));
	memcpy(c, "WMDR", 4);
	put_le32(c + 4, 16);
	uint32_t p = 16;
	struct {uint16_t off, type; uint32_t id, len;} blocks[4] = {
		{0, CSFW_NAME_T << 8, 0, 5}, {8, CSFW_XM_T, 0x5f201, 4}, {4, CSFW_YM_PACKED_T, 0x5f201, 12},
		{0x2c04, CSFW_ABSOLUTE_T << 8, 0, 4}};
	for (unsigned i = 0; i < 4; i++) {
		c[p] = (uint8_t)blocks[i].off;
		c[p + 1] = (uint8_t)(blocks[i].off >> 8);
		c[p + 2] = (uint8_t)blocks[i].type;
		c[p + 3] = (uint8_t)(blocks[i].type >> 8);
		put_le32(c + p + 4, blocks[i].id);
		put_le32(c + p + 16, blocks[i].len);
		p += (20 + blocks[i].len + 3) & ~3u;
	}
	CHECK(csfw_coeff_begin(&it, c, p) == CsfwOk);
	n = 0;
	while (csfw_next(&it, &info, &wr) == CsfwOk && n < 4) {
		regs[n++] = wr.reg;
	}
	CHECK(n == 3);
	CHECK(regs[0] == CSFW_XM + 0x200 * 4 + 8);
	CHECK(regs[1] == ((CSFW_YM_PACKED + 0x40 * 3) & ~3u) + 4);
	CHECK(regs[2] == 0x2c04);
	/* an algorithm the firmware does not have */
	put_le32(c + 16 + 28 + 4, 0x5f2ff);    /* the second block (after the 28-byte name block): its id */
	CHECK(csfw_coeff_begin(&it, c, p) == CsfwOk);
	int e;
	while ((e = csfw_next(&it, &info, &wr)) == CsfwOk) {
	}
	CHECK(e == CsfwNoAlg);
}

static uint8_t *
slurp(const char *path, uint32_t *size)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)n);
	if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
		free(b);
		b = 0;
	}
	fclose(fp);
	*size = (uint32_t)n;
	return b;
}

static void
phone(const char *dir, const char *dump)
{
	char path[1024];
	uint32_t size;
	snprintf(path, sizeof(path), "%s/cs35l43-dsp1-spk-prot.wmfw", dir);
	uint8_t *fw = slurp(path, &size);
	CHECK(fw != 0);
	if (!fw) {
		return;
	}
	CsfwInfo info;
	CHECK(csfw_wmfw_info(fw, size, &info) == CsfwOk);
	/* where the controls the driver uses are; the five with addresses were found in Android's
	 * register dumps holding the calibration file's values and the playback state */
	static const struct {
		uint32_t alg;
		const char *name;
		uint32_t reg;
	} ctls[] = {{0x5f212, "AUDIO_STATE", 0x280105c}, {0x5f224, "ALLOW_ACT_BOOT", 0x280125c},
	            {0x5f210, "CAL_R_SEL", 0x28033f4}, {0x5f210, "CAL_R_INIT", 0x2803278}, {0x5f210, "CAL_R", 0x280326c},
	            {0x1820d6, "HALO_STATE", 0}, {0x1820d6, "HALO_HEARTBEAT", 0}};
	uint32_t ctl_reg[sizeof(ctls) / sizeof(ctls[0])];
	for (unsigned i = 0; i < sizeof(ctls) / sizeof(ctls[0]); i++) {
		CsfwCtl c = {0, 0, 0, 0};
		const int e = csfw_find_ctl(fw, size, &info, ctls[i].alg, ctls[i].name, &c);
		printf("control %#x %-15s type %#x offset %#x %u bytes at %#x\n", ctls[i].alg, ctls[i].name, c.type, c.offset,
		    c.bytes, c.reg);
		CHECK(e == CsfwOk && (!ctls[i].reg || c.reg == ctls[i].reg));
		ctl_reg[i] = c.reg;
	}
	printf("firmware: vendor %u id %#x version %u.%u.%u, %u algorithms\n", info.vendor, info.fw_id, info.fw_ver >> 16,
	    info.fw_ver >> 8 & 0xff, info.fw_ver & 0xff, info.n_algs);
	CsfwIter it;
	CsfwWrite w;
	uint32_t bytes[256] = {0}, writes = 0;
	int e;
	CHECK(csfw_wmfw_begin(&it, fw, size) == CsfwOk);
	while ((e = csfw_next(&it, &info, &w)) == CsfwOk) {
		bytes[w.type & 0xff] += w.bytes;
		writes++;
	}
	CHECK(e == CsfwEnd);
	printf("firmware writes %u: PM %u bytes, XM packed %u, XM %u, YM packed %u, YM %u\n", writes,
	    bytes[CSFW_PM_PACKED_T], bytes[CSFW_XM_PACKED_T], bytes[CSFW_XM_T], bytes[CSFW_YM_PACKED_T], bytes[CSFW_YM_T]);
	static const char *const coeffs[] = {"dsp1", "delta-music", "delta-movie", "delta-video", "delta-navigation",
	                                     "delta-voice"};
	for (unsigned side = 0; side < 2; side++) {
		for (unsigned k = 0; k < 6; k++) {
			snprintf(path, sizeof(path), "%s/%c-cs35l43-%s-spk-prot.bin", dir, side ? 'B' : 'T', coeffs[k]);
			uint8_t *c = slurp(path, &size);
			CHECK(c != 0);
			if (!c) {
				continue;
			}
			unsigned blocks = 0, total = 0, wrong_ver = 0;
			CHECK(csfw_coeff_begin(&it, c, size) == CsfwOk);
			while ((e = csfw_next(&it, &info, &w)) == CsfwOk) {
				const CsfwAlg *a = csfw_find_alg(&info, w.alg);
				blocks++;
				total += w.bytes;
				/* the block's version field is the algorithm's version shifted by 8 */
				wrong_ver += a && w.ver != a->ver << 8;
			}
			CHECK(e == CsfwEnd);
			printf("%c %-17s %3u blocks, %5u bytes%s\n", side ? 'B' : 'T', coeffs[k], blocks, total,
			    e == CsfwEnd ? "" : " (stopped early)");
			CHECK(wrong_ver == 0);
			free(c);
		}
	}
	if (dump) {
		FILE *fp = fopen(dump, "r");
		CHECK(fp != 0);
		uint32_t want[10 + 6 * CSFW_MAX_ALGS + 1], got = 0;
		const unsigned n = 10 + 6 * info.n_algs + 1;
		uint32_t *hdr = &info.core_id;
		for (unsigned i = 0; i < 10; i++) {
			want[i] = i < 9 ? hdr[i] : info.n_algs;
		}
		for (unsigned k = 0; k < 6 * info.n_algs; k++) {
			want[10 + k] = (&info.algs[k / 6].id)[k % 6];
		}
		want[n - 1] = 0xbedead;
		char line[128];
		unsigned matched = 0;
		while (fp && fgets(line, sizeof(line), fp)) {
			unsigned long a, v;
			if (sscanf(line, "%lx: %lx", &a, &v) != 2) {
				continue;
			}
			if (a >= CSFW_XM && a < CSFW_XM + 4 * n) {
				got++;
				matched += want[(a - CSFW_XM) / 4] == (uint32_t)v;
			}
			for (unsigned i = 5; i < sizeof(ctls) / sizeof(ctls[0]); i++) {
				if (a == ctl_reg[i]) {
					printf("register dump: %s = %#lx\n", ctls[i].name, v);
				}
			}
		}
		if (fp) {
			fclose(fp);
		}
		printf("register dump: %u of %u id header words found, %u equal\n", got, n, matched);
		CHECK(got == n && matched == n);
	}
	free(fw);
}

int
main(int argc, char **argv)
{
	builtin();
	controls();
	if (argc > 1) {
		phone(argv[1], argc > 2 ? argv[2] : 0);
	}
	printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
