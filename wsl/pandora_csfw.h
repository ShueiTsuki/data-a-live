/*
 * Cirrus Logic Halo-core DSP files for the CS35L43 speaker amplifiers, as the register writes they
 * turn into. Two files per amplifier: the firmware (.wmfw, "WMFW" version 3, core 4 = Halo) and its
 * coefficients (.bin, "WMDR"); the coefficient blocks are placed relative to the firmware's
 * algorithms, whose list the firmware carries at XM word 0 (the id header the DSP itself reports).
 * File layouts as Linux's include/linux/firmware/cirrus/wmfw.h; register placement as cs_dsp.c's
 * cs_dsp_halo_region_to_reg and the CS35L41 family's DSP1 memory map. Checked against the phone's
 * own files: all 27 algorithms decode, every coefficient block of both amplifiers and all five
 * scene deltas resolves to one of them with the same version, and the decoded id header equals what
 * the amplifier holds at 0x2800000 during Android playback.
 * No allocation, no I/O: the caller hands in the file bytes and performs the writes.
 */
#ifndef PANDORA_CSFW_H
#define PANDORA_CSFW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The DSP1 memories in the amplifier's register space (32-bit registers, byte addresses). */
#define CSFW_PM_PACKED   0x03800000u   /* 40-bit instructions, 5 bytes each */
#define CSFW_XM_PACKED   0x02000000u   /* 24-bit words, 4 per 3 registers */
#define CSFW_YM_PACKED   0x02c00000u
#define CSFW_XM          0x02800000u   /* 24-bit words, one per register */
#define CSFW_YM          0x03400000u

/* Region and block types. */
#define CSFW_XM_T        0x05u
#define CSFW_YM_T        0x06u
#define CSFW_PM_PACKED_T 0x10u
#define CSFW_XM_PACKED_T 0x11u
#define CSFW_YM_PACKED_T 0x12u
#define CSFW_ABSOLUTE_T  0xf0u
#define CSFW_ALGDATA_T   0xf2u
#define CSFW_METADATA_T  0xfcu
#define CSFW_NAME_T      0xfeu
#define CSFW_INFO_T      0xffu

#define CSFW_MAX_ALGS    48u

enum {
	CsfwOk = 0, CsfwEnd = 1, CsfwBadMagic = -1, CsfwBadHeader = -2, CsfwBadRegion = -3, CsfwNoAlg = -4,
	CsfwTooMany = -5, CsfwShort = -6
};

typedef struct {
	uint32_t id, ver, xm_base, xm_size, ym_base, ym_size;   /* bases and sizes in 24-bit words */
} CsfwAlg;

typedef struct {
	uint32_t core_id, block_rev, vendor, fw_id, fw_ver, xm_base, xm_size, ym_base, ym_size;
	unsigned n_algs;
	CsfwAlg algs[CSFW_MAX_ALGS];
} CsfwInfo;

typedef struct {
	const uint8_t *file;
	uint32_t size, pos;
	int coeff;                     /* 0 a .wmfw, 1 a .bin */
} CsfwIter;

/* A firmware control: its register, memory type, word offset from its algorithm's base, length in bytes. */
typedef struct {
	uint32_t reg, type, offset, bytes;
} CsfwCtl;

/* One write: bytes at data to the registers from reg on (reg steps by 4 per 4 bytes). */
typedef struct {
	uint32_t reg, bytes, type, alg, ver;   /* alg, ver: a coefficient block's algorithm and its version field */
	const uint8_t *data;
} CsfwWrite;

/* The register of word OFFSET of memory TYPE (cs_dsp_halo_region_to_reg); 0 for a type with no memory. */
uint32_t csfw_region_reg(uint32_t type, uint32_t offset);
/* Four 24-bit words from three 32-bit registers as they are on the wire (big endian): word 0 in
 * the low bits of the first register. */
void csfw_unpack(const uint8_t b[12], uint32_t w[4]);

int csfw_wmfw_begin(CsfwIter *it, const uint8_t *file, uint32_t size);
/* The id header and algorithm list from the firmware's packed XM region at word 0. */
int csfw_wmfw_info(const uint8_t *file, uint32_t size, CsfwInfo *info);
int csfw_coeff_begin(CsfwIter *it, const uint8_t *file, uint32_t size);
/* The next write (text, names, metadata and algorithm descriptions are skipped); CsfwEnd at the
 * end. For a .bin, info places each block at its algorithm's base. */
int csfw_next(CsfwIter *it, const CsfwInfo *info, CsfwWrite *w);
const CsfwAlg *csfw_find_alg(const CsfwInfo *info, uint32_t id);
/* Control NAME (its short name, e.g. "AUDIO_STATE") of algorithm ALG, from the algorithm
 * descriptions in the .wmfw (file version 3); ALG may be the firmware's own id, whose controls sit
 * at the id header's bases. CsfwNoAlg if the algorithm or the control is not there. */
int csfw_find_ctl(const uint8_t *file, uint32_t size, const CsfwInfo *info, uint32_t alg, const char *name, CsfwCtl *ctl);

#ifdef __cplusplus
}
#endif

#endif
