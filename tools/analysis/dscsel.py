"""dscsel.py DSC VADDR...: the 8-byte pointer at each VADDR of a dyld shared cache (low 36 bits kept, as selrefs and
class refs are stored) and the C string it points to (run in WSL)."""
import glob
import struct
import sys

dsc = sys.argv[1]
maps = []
for path in [dsc] + sorted(glob.glob(dsc + '.*')):
    if path.endswith(('.map', '.symbols', '.a2s', '.cache')):
        continue
    with open(path, 'rb') as f:
        h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        moff, mcount = struct.unpack_from('<II', h, 0x10)
        f.seek(moff)
        for _ in range(mcount):
            addr, size, foff, maxp, initp = struct.unpack('<QQQII', f.read(32))
            maps.append((addr, size, foff, path))


def read(va, n):
    for addr, size, foff, path in maps:
        if addr <= va < addr + size:
            with open(path, 'rb') as f:
                f.seek(foff + va - addr)
                return f.read(n)
    return None


for a in sys.argv[2:]:
    va = int(a, 16)
    raw = read(va, 8)
    if raw is None:
        print('%x: not mapped' % va)
        continue
    p = struct.unpack('<Q', raw)[0]
    t = p & ((1 << 36) - 1)
    s = read(t, 120)
    print('%x: %x -> %x %s' % (va, p, t, s.split(b'\0')[0].decode('latin-1') if s else '(not mapped)'))
