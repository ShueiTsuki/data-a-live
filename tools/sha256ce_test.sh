#!/bin/bash
# The kernel's ARMv8 SHA-256 (wsl/pandora_sha256_ce.c) against Python's hashlib, under qemu-aarch64
# (apt: qemu-user-static). Run in WSL: bash tools/sha256ce_test.sh
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
clang-18 --target=aarch64-linux-gnu -march=armv8.2-a+crypto -O2 -ffreestanding -nostdlib -static \
	-fno-stack-protector -fuse-ld=lld -DPANDORA_SHA256_CE_TEST \
	"$R/tools/sha256ce_test.c" "$R/wsl/pandora_sha256_ce.c" -o "$T/t"
qemu-aarch64-static -cpu max "$T/t" > "$T/got"
python3 -I - "$T/got" <<'PY'
import hashlib, sys
x, big = 1, bytearray(1 << 20)
for i in range(len(big)):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    big[i] = x & 0xff
want = {'empty': b'', 'abc': b'abc', '448bits': b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
        '1MiB': bytes(big), '16KiB': bytes(big[4096:4096 + 16384]), '4KiB': bytes(big[8192:8192 + 4096])}
for n in range(1, 131):
    want['n%03d' % n] = bytes(big[:n])
got = dict(l.split() for l in open(sys.argv[1]))
bad = [k for k in want if got.get(k) != hashlib.sha256(want[k]).hexdigest()]
print('%d digests, %d wrong%s' % (len(want), len(bad), (': ' + ' '.join(bad[:8])) if bad else ''))
sys.exit(1 if bad else 0)
PY
rm -rf "$T"
