#!/bin/bash
# 0131: touch SPI over GPI DMA (SE0 is GSI-only), RPMh v3 layout for RSC v4.3, speaker amp
# I2C probe bsd/dev/pandora_amp.c (boot-arg pandora_amp=1).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
tr -d '\r' < $W/pandora_touch.c > bsd/dev/pandora_touch.c
tr -d '\r' < $W/pandora_amp.c > bsd/dev/pandora_amp.c
python3 - <<'PY'
def patch(p, subs):
    t = open(p).read()
    for old, new, what in subs:
        assert t.count(old) == 1, (p, what)
        t = t.replace(old, new)
    open(p, 'w').write(t)


patch('bsd/dev/sweet_ufs.c', [
    ("extern void pandora_touch_init(void);\n",
     "extern void pandora_touch_init(void);\nextern void pandora_amp_init(void);\n", "decl"),
    ("""	pandora_touch_init();
#endif""", """	pandora_touch_init();
	/* Speaker amplifiers, I2C probe only (bsd/dev/pandora_amp.c). */
	pandora_amp_init();
#endif""", "call"),
])
patch('bsd/conf/files', [
    ("bsd/dev/pandora_touch.c\t\t\tstandard\n",
     "bsd/dev/pandora_touch.c\t\t\tstandard\nbsd/dev/pandora_amp.c\t\t\tstandard\n", "files"),
])
print('patched')
PY
git add -A
git diff --cached --stat
git diff --cached > $W/../xnu/patches/0131-pandora-touch-gpi-rpmh-v4-amp-probe.patch
git -c user.name=t -c user.email=t@t commit -qm 0131
