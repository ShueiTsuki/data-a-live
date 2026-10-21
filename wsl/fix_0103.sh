#!/bin/bash
# Re-base UniXNU's native-4k-bootstrap-capacity onto the ios-fuxi start.s.
set -euo pipefail
cd /opt/work/tree
P=/mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0103-unixnu-native-4k-bootstrap-capacity.patch
sed -n 680,710p osfmk/arm64/start.s
# pmap.h and genassym.c apply unchanged
git apply --include=osfmk/arm/pmap/pmap.h --include=osfmk/arm64/genassym.c $P
python3 - <<'PY'
p='osfmk/arm64/start.s'
t=open(p).read()
a='''	/* allocate an L2 table */
3:	add	$4, $4, PGBYTES
'''
assert t.count(a)==1
t=t.replace(a,a+'''	/* Refuse before a new table can overwrite adjacent kernel globals. */
	add $6, x25, #(BOOTSTRAP_TABLE_BYTES >> 12), lsl #12
	cmp $4, $6
	b.hs Lbootstrap_tables_exhausted
''')
b='''	mov		x2, #(TTE_PGENTRIES)				// Load number of entries per page
	lsl		x2, x2, #2							// Shift by 2 for num entries on 4 pages
'''
assert t.count(b)==1
t=t.replace(b,'''	mov		x2, #(BOOTSTRAP_TABLE_BYTES / ARM_PGBYTES)
	lsl		x2, x2, #(ARM_PGSHIFT - TTE_SHIFT) // Clear the whole reserved pool
''')
c='	/* Ensure TTEs are visible */\n'
assert t.count(c)==1
t=t.replace(c,'''	b Lbootstrap_tables_ready
Lbootstrap_tables_exhausted:
	// MMU is still off. Stop with the offending cursor/end preserved for
	// the early debugger rather than corrupting memory or forging a map.
	b Lbootstrap_tables_exhausted
Lbootstrap_tables_ready:
'''+c)
open(p,'w').write(t)
PY
git diff > /tmp/0103.patch
git diff --stat
cp /tmp/0103.patch $P
git add -A && git -c user.name=t -c user.email=t@t commit -qm "unixnu 4k bootstrap (rebased)"
