"""dscstr.py DSC VADDR...: the C string at each VADDR of a dyld shared cache (run in WSL)."""
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
for a in sys.argv[2:]:
    va = int(a, 16)
    for addr, size, foff, path in maps:
        if addr <= va < addr + size:
            with open(path, 'rb') as f:
                f.seek(foff + va - addr)
                print('%x: %s' % (va, f.read(160).split(b'\0')[0].decode('latin-1')))
            break
    else:
        print('%x: not mapped' % va)
