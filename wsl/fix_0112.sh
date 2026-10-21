#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'osfmk/vm/vm_compressor.c'
t = open(p).read()
old = """	compressor_pool_max_size = C_SEG_MAX_LIMIT;
	compressor_pool_max_size *= c_seg_bufsize;

#if XNU_TARGET_OS_OSX
"""
new = """	compressor_pool_max_size = C_SEG_MAX_LIMIT;
	compressor_pool_max_size *= c_seg_bufsize;

#if XNU_TARGET_OS_OSX && !defined(PANDORA)
"""
assert t.count(old) == 1
t = t.replace(old, new)
old = """#else

	if (compressor_pool_max_size > max_mem) {
		compressor_pool_max_size = max_mem;
	}
"""
new = """#else
	/* iOS sizing; also PANDORA (macOS-platform build on the iOS 4K kernel VA layout):
	 * the macOS pool of 8 x max_mem (40 GiB VA for 5 GiB RAM) plus the 24 GiB zone map
	 * does not fit in the kernel VA and kmem_range_init panics. */

	if (compressor_pool_max_size > max_mem) {
		compressor_pool_max_size = max_mem;
	}
"""
assert t.count(old) == 1
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0112-pandora-ios-compressor-pool-size.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0112
