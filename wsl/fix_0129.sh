#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)


sub("static void sweet_log_start(void);\n", """static void sweet_log_start(void);
#if defined(PANDORA)
static void pandora_log_start(void);
#endif
""", "fwd")

sub("""static uint64_t fx_part_first, fx_part_blocks;
""", """static uint64_t fx_part_first, fx_part_blocks;
#if defined(PANDORA)
#define PL_MAX_EXTENTS 64
static struct { uint64_t file, part, count; } pl_ext[PL_MAX_EXTENTS];
static unsigned pl_next;
static uint64_t pl_blocks;
static bool pl_inside(uint64_t lba, uint64_t count);
static bool pl_map(uint64_t blk, uint64_t *lba, uint64_t *run);
#endif
""", "table")

# fx_write_allowed: also the log file's blocks.
i = t.index("fx_write_allowed(uint64_t lba, uint64_t count)\n{")
j = t.index("	if (!fx_root || count == 0) {\n		return false;\n	}\n", i)
k = j + len("	if (!fx_root || count == 0) {\n		return false;\n	}\n")
t = t[:k] + """#if defined(PANDORA)
	if (pl_inside(lba, count)) {
		return true;
	}
#endif
""" + t[k:]

# pl_* helpers right after fx_load_extents.
i = t.index("fx_load_extents(void)\n{")
j = t.index("\tfx_root = true;\n\treturn 0;\n}\n", i) + len("\tfx_root = true;\n\treturn 0;\n}\n")
t = t[:j] + open(W + 'pandora_logext.c.txt').read() + t[j:]

sub("""		error = fx_load_extents();
		if (!error) {""", """		error = fx_load_extents();
#if defined(PANDORA)
		if (!error) {
			pl_load_extents();
		}
#endif
		if (!error) {""", "load")

sub("""	/* Touch (bsd/dev/fuxi_touch.c; rails voted by the loader). */
	fuxi_touch_init();""", """#if defined(PANDORA)
	pandora_log_start();
#endif
	/* Touch (bsd/dev/fuxi_touch.c; rails voted by the loader). */
	fuxi_touch_init();""", "start")

# Log writer + restart hook after sweet_log_start's definition.
i = t.index("static void\nsweet_log_start(void)\n{")
j = t.index("\n}\n", i) + 3
t = t[:j] + "\n" + open(W + 'pandora_log.c.txt').read() + t[j:]
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0129-pandora-log-file-and-psci-restart.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0129
