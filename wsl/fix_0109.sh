#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p='osfmk/kern/zalloc.c'
t=open(p).read()
old='''#if XNU_PLATFORM_MacOSX
#define ZONE_MAP_MAX            (32ULL << 30)
#define ZONE_MAP_VA_SIZE        (128ULL << 30)
#else /* XNU_PLATFORM_MacOSX */'''
assert t.count(old)==1
new='''#if XNU_PLATFORM_MacOSX && !defined(PANDORA)
#define ZONE_MAP_MAX            (32ULL << 30)
#define ZONE_MAP_VA_SIZE        (128ULL << 30)
#else /* XNU_PLATFORM_MacOSX */
/* iOS sizes; also PANDORA, whose iOS kernel VA layout (no ARM_LARGE_MEMORY,
 * ~112 GiB of kernel VA) cannot hold a 128 GiB zone map. */'''
t=t.replace(old,new); open(p,'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0109-pandora-ios-zone-map-size.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0109
