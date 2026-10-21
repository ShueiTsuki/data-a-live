#!/bin/bash
# sym.sh KERNEL ADDR... : nearest preceding symbol for each address
K=$1; shift
/opt/swift/usr/bin/llvm-nm -n "$K" | grep -E '^[0-9a-f]{16} [tTsSdD] ' > /tmp/syms.txt
for a in "$@"; do
  python3 - "$a" <<'PY'
import sys
t=int(sys.argv[1],16); best=None
for l in open('/tmp/syms.txt'):
    v=int(l[:16],16)
    if v<=t: best=l.strip()
    else: break
print(sys.argv[1], '->', best, '+0x%x' % (t-int(best[:16],16)) if best else '')
PY
done
