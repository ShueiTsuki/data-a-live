#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p='pexpert/arm/pe_serial.c'
t=open(p).read()
old='''    fuxi_ctl0 = (volatile uint32_t *)ml_io_map(0x0ae16000, 0x1000);
    fuxi_intf1 = (volatile uint32_t *)ml_io_map(0x0ae36000, 0x1000);
'''
assert t.count(old)==1
new='''#if !defined(PANDORA)
    fuxi_ctl0 = (volatile uint32_t *)ml_io_map(0x0ae16000, 0x1000);
    fuxi_intf1 = (volatile uint32_t *)ml_io_map(0x0ae36000, 0x1000);
#endif
    /* PANDORA: SM8550 MDP addresses do not exist here (SM8850 MDP is at 0x9800000) and
     * the 17 Pro panel refreshes by itself (measured by shim/hello), so no kick:
     * fuxi_display_kick() returns early while fuxi_intf1 is NULL. */
'''
t=t.replace(old,new); open(p,'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0108-pandora-no-sm8550-display-kick.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0108
