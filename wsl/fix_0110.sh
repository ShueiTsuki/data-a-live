#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p='iokit/KernelConfigTables.cpp'
t=open(p).read()
for old,new in [
 ("""#if defined(FUXI)
    "     'IONameMatch'     = 'xiaomi,fuxi';\"""", """#if defined(PANDORA)
    "     'IONameMatch'     = 'xiaomi,pandora';"
#elif defined(FUXI)
    "     'IONameMatch'     = 'xiaomi,fuxi';\""""),
 ("""#if defined(FUXI)
    "     'IONameMatch'     = 'qcom,sm8550-cpu';\"""", """#if defined(PANDORA)
    "     'IONameMatch'     = 'qcom,sm8850-cpu';"
#elif defined(FUXI)
    "     'IONameMatch'     = 'qcom,sm8550-cpu';\"""")]:
    assert t.count(old)==1, old
    t=t.replace(old,new)
open(p,'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0110-pandora-platform-names.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0110
