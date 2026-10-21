#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
for p, arg, msg in [('bsd/dev/fuxi_touch.c', 'fuxitouch', None),
                    ('bsd/dev/fuxi_keys.c', 'fuxikeys', 'FUXI KEYS: disabled (fuxikeys=0)')]:
    t = open(p).read()
    old = "\tint enabled = 1;\n\tthread_t thread;\n"
    new = ("#if defined(PANDORA)\n"
           "\t/* Xiaomi 13 (SM8550) register addresses; the 17 Pro's differ. Off unless %s=1. */\n"
           "\tint enabled = 0;\n#else\n\tint enabled = 1;\n#endif\n\tthread_t thread;\n") % arg
    assert t.count(old) == 1, p
    t = t.replace(old, new)
    if msg:
        assert t.count(msg) == 1
        t = t.replace(msg, 'FUXI KEYS: disabled')
    else:
        old = '\tif (!enabled) {\n\t\treturn;\n\t}\n\tif (kernel_thread_start_priority(ft_thread'
        assert t.count(old) == 1
        t = t.replace(old, '\tif (!enabled) {\n\t\tprintf("FUXI TOUCH: disabled' + chr(92) + 'n");\n\t\treturn;\n\t}\n\tif (kernel_thread_start_priority(ft_thread')
    open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0111-pandora-sm8550-input-drivers-off.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0111
