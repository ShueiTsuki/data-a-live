"""Host test of menu.c's FDT code (chosen_prop, has_word, normal_mode, layout_ok).

    python -I test/fdt_test.py [DTB]     (default device/pandora-live.dtb, ABL's FDT from Android)

The functions are cut out of menu.c as they are (from "---- ABL's FDT" to "---- slot checksum"),
compiled for the PC with gcc in WSL and run against the live FDT and edited copies of it:
charger and silent boot words, a bad magic, no initrd, the image overlapping the initrd.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'menu.c')
DTB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'device', 'pandora-live.dtb')
# Copies of the phone's FDT stay outside the repo.
WORK = tempfile.mkdtemp(prefix='bootmgr_fdt_')

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t rd8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }
static uint32_t be32(uint64_t a)
{
	return (uint32_t)rd8(a) << 24 | (uint32_t)rd8(a + 1) << 16 | (uint32_t)rd8(a + 2) << 8 | rd8(a + 3);
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
@@FUNCS@@
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	static uint8_t buf[16 << 20] __attribute__((aligned(8)));
	size_t n = fread(buf, 1, sizeof(buf), f);
	(void)n;
	uint64_t fdt = (uintptr_t)buf;
	uint32_t l = 0, ls = 0;
	uint64_t s = chosen_prop(fdt, "linux,initrd-start", &ls);
	uint64_t is = s ? ((uint64_t)be32(s) << 32 | be32(s + 4)) : 0;
	(void)chosen_prop(fdt, "bootargs", &l);
	/* an image far from the initrd, and one covering its first page */
	printf("bootargs_len=%u normal=%d layout_far=%d layout_overlap=%d\n", l, normal_mode(fdt),
	    layout_ok(fdt, 0x880000000ull, 0x6000000ull), is ? layout_ok(fdt, is - 0x1000, 0x2000) : -1);
	const char *t = "a bootmonitor.bootmode=normal b";
	printf("word=%d prefix=%d suffix=%d\n", has_word((uintptr_t)t, (uint32_t)strlen(t), "bootmonitor.bootmode=normal"),
	    has_word((uintptr_t)t, (uint32_t)strlen(t), "monitor.bootmode=normal"),
	    has_word((uintptr_t)t, (uint32_t)strlen(t), "bootmonitor.bootmode=norm"));
	return 0;
}
'''


def wsl(path):
    path = os.path.abspath(path).replace('\\', '/')
    return '/mnt/' + path[0].lower() + path[2:]


def run(dtb_bytes, tag):
    p = os.path.join(WORK, tag + '.dtb')
    open(p, 'wb').write(dtb_bytes)
    r = subprocess.run(['wsl', '-d', 'Ubuntu-24.04', '--', wsl(os.path.join(WORK, 'fdt_test')), wsl(p)],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    return r.stdout.strip() + r.stderr.strip()


def main():
    src = open(SRC, encoding='utf-8').read()
    m = re.search(r'/\* ---- ABL\'s FDT.*?(?=/\* ---- slot checksum)', src, re.S)
    assert m, 'FDT section not found in menu.c'
    c = os.path.join(WORK, 'fdt_test.c')
    open(c, 'w', newline='\n').write(HARNESS.replace('@@FUNCS@@', m.group(0)))
    r = subprocess.run(['wsl', '-d', 'Ubuntu-24.04', '--', 'gcc', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-o', wsl(os.path.join(WORK, 'fdt_test')), wsl(c)],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    assert r.returncode == 0, r.stdout + r.stderr

    live = open(DTB, 'rb').read()
    cases = [('live', live, 'normal=1 layout_far=1 layout_overlap=0')]
    for old, new in ((b'mca_log.charge_boot_mode=0', b'mca_log.charge_boot_mode=1'),
                     (b'bootmonitor.bootmode=normal', b'bootmonitor.bootmode=charge'),
                     (b'silent_boot.mode=nonsilent', b'silent_boot.mode=xxsilent')):
        assert live.count(old) == 1, old
        cases.append((new.decode(), live.replace(old, new.ljust(len(old), b' ')), 'normal=0'))
    cases.append(('bad magic', b'\0' + live[1:], 'normal=0 layout_far=0'))
    assert live.count(b'linux,initrd-start') == 1
    cases.append(('no initrd', live.replace(b'linux,initrd-start', b'linux,initrd-strat'), 'layout_far=0'))
    bad = 0
    for tag, data, want in cases:
        got = run(data, re.sub(r'\W', '_', tag))
        ok = all(w in got.split() for w in want.split()) and 'word=1 prefix=0 suffix=0' in got
        bad += not ok
        print(('ok  ' if ok else 'BAD ') + f'{tag}: {got}')
    shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
