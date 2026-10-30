"""dscdis.py DSC VADDR [BYTES]: disassemble BYTES (default 0x300) at VADDR of a dyld shared cache (run in WSL).

Maps the address through the cache's mappings (dyld_cache_header mappingOffset/mappingCount, each
{address, size, fileOffset, maxProt, initProt}) in the main file or its .01/.02... subcaches, then llvm-mc.
"""
import glob
import struct
import subprocess
import sys

dsc, va = sys.argv[1], int(sys.argv[2], 16)
n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x300
for path in [dsc] + sorted(glob.glob(dsc + '.*')):
    if path.endswith('.map') or path.endswith('.symbols') or path.endswith('.a2s'):
        continue
    with open(path, 'rb') as f:
        h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        moff, mcount = struct.unpack_from('<II', h, 0x10)
        f.seek(moff)
        for _ in range(mcount):
            addr, size, foff, maxp, initp = struct.unpack('<QQQII', f.read(32))
            if addr <= va < addr + size:
                f.seek(foff + va - addr)
                data = f.read(n)
                hexs = ' '.join('0x%02x' % b for b in data)
                out = subprocess.run(['llvm-mc', '--disassemble', '-triple=arm64-apple-ios', '-mattr=+v8.5a'],
                                     input=hexs, capture_output=True, text=True).stdout
                a = va
                for line in out.splitlines():
                    line = line.strip()
                    if not line or line.startswith('.'):
                        continue
                    print('%x: %s' % (a, line))
                    a += 4
                sys.exit(0)
sys.exit('address not in any mapping')
