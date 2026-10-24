"""menu.c's bm_sum (cut out as it is, built for the PC in WSL) against slot.py's bm_sum.

    python -I test/sum_test.py [SLOT]     (default out/slot.img: its Image and stored sum)

A mismatch would make the menu call every slot damaged. Also checks a few short inputs.
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import slot  # noqa: E402

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
@@FUNCS@@
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint64_t *buf = malloc((size_t)n + 8);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
		return 3;
	printf("%016llx\n", (unsigned long long)bm_sum((uintptr_t)buf, (uint64_t)n));
	return 0;
}
'''


def wsl(path):
    path = os.path.abspath(path).replace('\\', '/')
    return '/mnt/' + path[0].lower() + path[2:]


def sh(*cmd):
    r = subprocess.run(['wsl', '-d', 'Ubuntu-24.04', '--', *cmd], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout.strip()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'out', 'slot.img')
    src = open(os.path.join(HERE, '..', 'menu.c'), encoding='utf-8').read()
    m = re.search(r'/\* ---- slot checksum.*?(?=/\* ---- the iOS slot)', src, re.S)
    assert m, 'checksum section not found in menu.c'
    work = tempfile.mkdtemp(prefix='bootmgr_sum_')
    try:
        c = os.path.join(work, 'sum.c')
        open(c, 'w', newline='\n').write(HARNESS.replace('@@FUNCS@@', m.group(0)))
        sh('gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-o', wsl(os.path.join(work, 'sum')), wsl(c))
        cases = []
        rnd = random.Random(1)
        for n in (0, 8, 16, 4096, 4104):
            cases.append((f'random {n}', bytes(rnd.getrandbits(8) for _ in range(n)), None))
        cases.append(('all ones 64', b'\xff' * 64, None))
        data = open(path, 'rb').read()
        size, stored = struct.unpack_from('<QQ', data, 16)
        cases.append((os.path.basename(path), data[4096:4096 + size], stored))
        bad = 0
        for tag, blob, stored in cases:
            p = os.path.join(work, 'in.bin')
            open(p, 'wb').write(blob)
            c_sum = int(sh(wsl(os.path.join(work, 'sum')), wsl(p)), 16)
            py_sum = slot.bm_sum(blob)
            ok = c_sum == py_sum and (stored is None or stored == c_sum)
            bad += not ok
            print(('ok  ' if ok else 'BAD ') + f'{tag}: C {c_sum:016x} python {py_sum:016x}'
                  + (f' stored {stored:016x}' if stored is not None else ''))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
