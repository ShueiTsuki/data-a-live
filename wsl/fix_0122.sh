#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()


def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)


sub("""#define FX_MAX_EXTENTS 512
static struct { uint64_t file, part, count; } fx_ext[FX_MAX_EXTENTS];
""", """#if defined(PANDORA)
/* 17 Pro: a 16 GiB pinned file on a phone in use spans ~4500 f2fs sections (2 MiB
 * each, not adjacent). fx_ext is in file order (the loader requires it); fx_by_part
 * indexes it in partition order, so both lookups are binary searches. */
#define FX_MAX_EXTENTS 16384
#else
#define FX_MAX_EXTENTS 512
#endif
static struct { uint64_t file, part, count; } fx_ext[FX_MAX_EXTENTS];
#if defined(PANDORA)
static uint16_t fx_by_part[FX_MAX_EXTENTS];
#endif
""", "max")

sub("""fx_map(uint64_t lblk, uint64_t *lba, uint64_t *run)
{
	for (unsigned i = 0; i < fx_next; i++) {""", """fx_map(uint64_t lblk, uint64_t *lba, uint64_t *run)
{
#if defined(PANDORA)
	unsigned lo = 0, hi = fx_next;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		if (lblk < fx_ext[mid].file) {
			hi = mid;
		} else if (lblk - fx_ext[mid].file >= fx_ext[mid].count) {
			lo = mid + 1;
		} else {
			uint64_t off = lblk - fx_ext[mid].file;
			*lba = fx_part_first + fx_ext[mid].part + off;
			*run = fx_ext[mid].count - off;
			return true;
		}
	}
	return false;
#endif
	for (unsigned i = 0; i < fx_next; i++) {""", "map")

sub("""	if (!fx_root || count == 0) {
		return false;
	}
	for (unsigned i = 0; i < fx_next; i++) {""", """	if (!fx_root || count == 0) {
		return false;
	}
#if defined(PANDORA)
	if (lba < fx_part_first) {
		return false;
	}
	uint64_t rel = lba - fx_part_first;
	unsigned lo = 0, hi = fx_next;          /* first entry whose part is > rel */
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		if (fx_ext[fx_by_part[mid]].part <= rel) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return false;
	}
	uint64_t first = fx_ext[fx_by_part[lo - 1]].part, n = fx_ext[fx_by_part[lo - 1]].count;
	return rel - first < n && count <= n - (rel - first);
#endif
	for (unsigned i = 0; i < fx_next; i++) {""", "write")

sub("""		count = MIN(count, want - pos);
		for (unsigned j = 0; j < fx_next; j++) {
			if (part < fx_ext[j].part + fx_ext[j].count && fx_ext[j].part < part + count) {
				printf("FUXI UFS: overlapping extents %u/%u\\n", j, i);
				return EINVAL;
			}
		}
""", """		count = MIN(count, want - pos);
#if !defined(PANDORA)
		for (unsigned j = 0; j < fx_next; j++) {
			if (part < fx_ext[j].part + fx_ext[j].count && fx_ext[j].part < part + count) {
				printf("FUXI UFS: overlapping extents %u/%u\\n", j, i);
				return EINVAL;
			}
		}
#endif
""", "overlap")

sub("""	if (pos != want) {
		printf("FUXI UFS: extents cover 0x%llx of 0x%llx blocks\\n", pos, want);
		fx_next = 0;
		return EINVAL;
	}
	fx_root = true;
	return 0;
}""", """	if (pos != want) {
		printf("FUXI UFS: extents cover 0x%llx of 0x%llx blocks\\n", pos, want);
		fx_next = 0;
		return EINVAL;
	}
#if defined(PANDORA)
	/* Partition-order index (Shell sort), then no two neighbours may overlap. */
	for (unsigned i = 0; i < fx_next; i++) {
		fx_by_part[i] = (uint16_t)i;
	}
	for (unsigned gap = fx_next / 2; gap > 0; gap /= 2) {
		for (unsigned i = gap; i < fx_next; i++) {
			uint16_t v = fx_by_part[i];
			unsigned j = i;
			while (j >= gap && fx_ext[fx_by_part[j - gap]].part > fx_ext[v].part) {
				fx_by_part[j] = fx_by_part[j - gap];
				j -= gap;
			}
			fx_by_part[j] = v;
		}
	}
	for (unsigned i = 1; i < fx_next; i++) {
		unsigned a = fx_by_part[i - 1], b = fx_by_part[i];
		if (fx_ext[a].part + fx_ext[a].count > fx_ext[b].part) {
			printf("FUXI UFS: overlapping extents %u/%u\\n", a, b);
			fx_next = 0;
			return EINVAL;
		}
	}
#endif
	fx_root = true;
	return 0;
}""", "sort")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0122-pandora-extent-table-16k-binary-search.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0122
