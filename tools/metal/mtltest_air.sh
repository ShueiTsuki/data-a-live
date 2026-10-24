#!/bin/bash
# Turn a CI-built mtltest's shaders into tools/metal/mtltest.air.ll, the AIR text build_plugin_linux.sh assembles:
# the metallib embedded in the binary holds one AIR bitcode module per function; they are disassembled and linked
# into one module with Rust's llvm-tools (LLVM 21+ reads Apple's AIR). Run in WSL after mtltest.metal changes and
# CI has built it; later small changes can be made in the .ll by hand.
#
#   tools/metal/mtltest_air.sh MTLTEST_BINARY
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(rustup run 1.98.0 rustc --print sysroot)/lib/rustlib/x86_64-unknown-linux-gnu/bin
W=$(mktemp -d)
python3 -I - "$1" "$W" <<'EOF'
import os, struct, sys
data = open(sys.argv[1], 'rb').read()
at = data.find(b'MTLB')
assert at >= 0, 'no metallib in the binary'
lib = data[at:at + struct.unpack_from('<Q', data, at + 16)[0]]
n = i = 0
while (i := lib.find(b'\xde\xc0\x17\x0b', i)) >= 0:   # bitcode wrapper: magic, version, offset, size
    offset, size = struct.unpack_from('<II', lib, i + 8)
    open(os.path.join(sys.argv[2], 'f%02d.bc' % n), 'wb').write(lib[i:i + offset + size])
    n += 1
    i += 4
print(n, 'functions')
EOF
"$T/llvm-link" "$W"/f*.bc -o "$W/linked.bc"
{
	echo "; mtltest's shaders as AIR (tools/metal/mtltest.metal compiled by Apple's metal on CI, then"
	echo "; disassembled and linked by tools/metal/mtltest_air.sh). build_plugin_linux.sh assembles it."
	"$T/llvm-dis" "$W/linked.bc" -o -
} > "$ROOT/tools/metal/mtltest.air.ll"
grep -c '^define' "$ROOT/tools/metal/mtltest.air.ll"
rm -rf "$W"
