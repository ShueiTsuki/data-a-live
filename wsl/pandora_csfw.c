/*
 * Cirrus Logic Halo-core DSP files as register writes (pandora_csfw.h).
 */
#include "pandora_csfw.h"

#define WMFW_HEADER   12u     /* magic, header length, rev (16), core, version */
#define WMFW_SIZES    16u     /* xm, ym, pm, zm */
#define WMFW_FOOTER   12u     /* timestamp (64), checksum */
#define WMFW_REGION   8u      /* offset | type << 24, length */
#define WMDR_HEADER   16u     /* magic, header length, version, core */
#define WMDR_BLOCK    20u     /* offset (16), type (16), id, version, sample rate, length */
#define ID_TERMINATOR 0xbedeadu

_Static_assert(sizeof(CsfwAlg) == 6 * sizeof(uint32_t), "CsfwAlg is six words, filled in order");

static uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

uint32_t
csfw_region_reg(uint32_t type, uint32_t offset)
{
	switch (type) {
	case CSFW_XM_T:
		return CSFW_XM + offset * 4u;
	case CSFW_YM_T:
		return CSFW_YM + offset * 4u;
	case CSFW_XM_PACKED_T:
		return (CSFW_XM_PACKED + offset * 3u) & ~3u;
	case CSFW_YM_PACKED_T:
		return (CSFW_YM_PACKED + offset * 3u) & ~3u;
	case CSFW_PM_PACKED_T:
		return CSFW_PM_PACKED + offset * 5u;
	default:
		return 0;
	}
}

void
csfw_unpack(const uint8_t b[12], uint32_t w[4])
{
	const uint32_t r0 = be32(b), r1 = be32(b + 4), r2 = be32(b + 8);
	w[0] = r0 & 0xffffffu;
	w[1] = (r0 >> 24) | (r1 & 0xffffu) << 8;
	w[2] = (r1 >> 16) | (r2 & 0xffu) << 16;
	w[3] = r2 >> 8;
}

int
csfw_wmfw_begin(CsfwIter *it, const uint8_t *file, uint32_t size)
{
	it->file = file;
	it->size = size;
	it->pos = 0;
	it->coeff = 0;
	if (size < WMFW_HEADER + WMFW_SIZES + WMFW_FOOTER) {
		return CsfwShort;
	}
	if (file[0] != 'W' || file[1] != 'M' || file[2] != 'F' || file[3] != 'W') {
		return CsfwBadMagic;
	}
	/* core 4 (Halo) at byte 10, file version 3 at byte 11 */
	if (le32(file + 4) != WMFW_HEADER + WMFW_SIZES + WMFW_FOOTER || file[10] != 4 || file[11] != 3) {
		return CsfwBadHeader;
	}
	it->pos = le32(file + 4);
	return CsfwOk;
}

int
csfw_coeff_begin(CsfwIter *it, const uint8_t *file, uint32_t size)
{
	it->file = file;
	it->size = size;
	it->pos = 0;
	it->coeff = 1;
	if (size < WMDR_HEADER) {
		return CsfwShort;
	}
	if (file[0] != 'W' || file[1] != 'M' || file[2] != 'D' || file[3] != 'R') {
		return CsfwBadMagic;
	}
	const uint32_t hl = le32(file + 4);
	if (hl < WMDR_HEADER || hl > size) {
		return CsfwBadHeader;
	}
	it->pos = hl;
	return CsfwOk;
}

const CsfwAlg *
csfw_find_alg(const CsfwInfo *info, uint32_t id)
{
	for (unsigned i = 0; info && i < info->n_algs; i++) {
		if (info->algs[i].id == id) {
			return &info->algs[i];
		}
	}
	return 0;
}

static int
next_wmfw(CsfwIter *it, CsfwWrite *w)
{
	while (it->pos < it->size) {
		if (it->size - it->pos < WMFW_REGION) {
			return CsfwShort;
		}
		const uint8_t *r = it->file + it->pos;
		const uint32_t word = le32(r), bytes = le32(r + 4), type = word >> 24, offset = word & 0xffffffu;
		if (bytes > it->size - it->pos - WMFW_REGION) {
			return CsfwShort;
		}
		it->pos += WMFW_REGION + bytes;
		switch (type) {
		case CSFW_INFO_T:
		case CSFW_NAME_T:
		case CSFW_METADATA_T:
		case CSFW_ALGDATA_T:
			continue;
		case CSFW_ABSOLUTE_T:
			w->reg = offset;
			break;
		case CSFW_XM_T:
		case CSFW_YM_T:
		case CSFW_XM_PACKED_T:
		case CSFW_YM_PACKED_T:
		case CSFW_PM_PACKED_T:
			w->reg = csfw_region_reg(type, offset);
			break;
		default:
			return CsfwBadRegion;
		}
		if (bytes & 3u) {
			return CsfwBadRegion;          /* whole 32-bit registers only */
		}
		w->type = type;
		w->alg = w->ver = 0;
		w->data = r + WMFW_REGION;
		w->bytes = bytes;
		return CsfwOk;
	}
	return CsfwEnd;
}

static int
next_coeff(CsfwIter *it, const CsfwInfo *info, CsfwWrite *w)
{
	while (it->pos < it->size) {
		if (it->size - it->pos < WMDR_BLOCK) {
			return CsfwShort;
		}
		const uint8_t *b = it->file + it->pos;
		const uint32_t offset = le16(b), type = le16(b + 2), id = le32(b + 4), bytes = le32(b + 16);
		if (bytes > it->size - it->pos - WMDR_BLOCK) {
			return CsfwShort;
		}
		it->pos += (WMDR_BLOCK + bytes + 3u) & ~3u;
		if (it->pos > it->size) {
			it->pos = it->size;
		}
		/* text, names and metadata carry their type in the upper byte here (cs_dsp_load_coeff) */
		if (type == CSFW_NAME_T << 8 || type == CSFW_INFO_T << 8 || type == CSFW_METADATA_T << 8) {
			continue;
		}
		if (type == CSFW_ABSOLUTE_T << 8) {
			w->reg = offset;
		} else if (type == CSFW_XM_T || type == CSFW_YM_T || type == CSFW_XM_PACKED_T || type == CSFW_YM_PACKED_T) {
			const CsfwAlg *a = csfw_find_alg(info, id);
			if (!a) {
				return CsfwNoAlg;
			}
			const uint32_t base = (type == CSFW_YM_T || type == CSFW_YM_PACKED_T) ? a->ym_base : a->xm_base;
			w->reg = csfw_region_reg(type, base) + offset;
		} else {
			return CsfwBadRegion;
		}
		if (bytes & 3u) {
			return CsfwBadRegion;
		}
		w->type = type;
		w->alg = id;
		w->ver = le32(b + 8);
		w->data = b + WMDR_BLOCK;
		w->bytes = bytes;
		return CsfwOk;
	}
	return CsfwEnd;
}

int
csfw_next(CsfwIter *it, const CsfwInfo *info, CsfwWrite *w)
{
	return it->coeff ? next_coeff(it, info, w) : next_wmfw(it, w);
}

int
csfw_wmfw_info(const uint8_t *file, uint32_t size, CsfwInfo *info)
{
	CsfwIter it;
	CsfwWrite w;
	int e = csfw_wmfw_begin(&it, file, size);
	if (e != CsfwOk) {
		return e;
	}
	const uint8_t *xm0 = 0;
	uint32_t xm0_bytes = 0;
	while ((e = next_wmfw(&it, &w)) == CsfwOk) {
		if (w.type == CSFW_XM_PACKED_T && w.reg == CSFW_XM_PACKED) {
			xm0 = w.data;
			xm0_bytes = w.bytes;
			break;
		}
	}
	if (!xm0) {
		return e < 0 ? e : CsfwNoAlg;
	}
	/* the words as the DSP reads them: id header (10 words), 6 per algorithm, then the terminator */
	const uint32_t nwords = xm0_bytes / 12u * 4u;
	uint32_t hdr[10];
	if (nwords < 11) {
		return CsfwShort;
	}
	for (unsigned i = 0; i < 10; i++) {
		uint32_t q[4];
		csfw_unpack(xm0 + i / 4u * 12u, q);
		hdr[i] = q[i % 4u];
	}
	info->core_id = hdr[0];
	info->block_rev = hdr[1];
	info->vendor = hdr[2];
	info->fw_id = hdr[3];
	info->fw_ver = hdr[4];
	info->xm_base = hdr[5];
	info->xm_size = hdr[6];
	info->ym_base = hdr[7];
	info->ym_size = hdr[8];
	info->n_algs = hdr[9];
	if (info->n_algs > CSFW_MAX_ALGS) {
		return CsfwTooMany;
	}
	if (10u + 6u * info->n_algs + 1u > nwords) {
		return CsfwShort;
	}
	for (unsigned k = 0; k <= 6u * info->n_algs; k++) {
		const unsigned i = 10u + k;
		uint32_t q[4];
		csfw_unpack(xm0 + i / 4u * 12u, q);
		if (k == 6u * info->n_algs) {
			if (q[i % 4u] != ID_TERMINATOR) {
				return CsfwBadHeader;
			}
			break;
		}
		uint32_t *f = &info->algs[k / 6u].id;
		f[k % 6u] = q[i % 4u];
	}
	return CsfwOk;
}
