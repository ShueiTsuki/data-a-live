"""dscfind.py DSC STRING: which images of a dyld shared cache hold STRING (C string), by mapping each hit's file
offset to a virtual address and that address to the image whose text range contains it (run in WSL)."""
import glob
import mmap
import struct
import sys

dsc, needle = sys.argv[1], sys.argv[2].encode() + b'\0'
files = [dsc] + sorted(p for p in glob.glob(dsc + '.*') if p[-3:-2] != '.' or p[-2:].isdigit())
images = []      # (start, end, path) from the main cache's image list and each image's __TEXT size
maps = []        # (path, address, size, fileoff)
for path in files:
    with open(path, 'rb') as f:
        h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        moff, mcount = struct.unpack_from('<II', h, 0x10)
        f.seek(moff)
        for _ in range(mcount):
            addr, size, foff, _, _ = struct.unpack('<QQQII', f.read(32))
            maps.append((path, addr, size, foff))
        if path == dsc:
            # imagesOffset/imagesCount (new layout at 0x1c0/0x1c4; old at 0x18/0x1c)
            ioff, icount = struct.unpack_from('<II', h, 0x1c0)
            if icount == 0:
                ioff, icount = struct.unpack_from('<II', h, 0x18)
            f.seek(ioff)
            recs = [struct.unpack('<QQQII', f.read(32)) for _ in range(icount)]
            for addr, _, _, poff, _ in recs:
                f.seek(poff)
                name = b''
                while True:
                    c = f.read(64)
                    i = c.find(b'\0')
                    if i >= 0:
                        name += c[:i]
                        break
                    name += c
                images.append((addr, name.decode(errors='replace')))
images.sort()
hits = {}
for path, addr, size, foff in maps:
    with open(path, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        i = mm.find(needle, foff, foff + size)
        while i >= 0:
            va = addr + i - foff
            # the image with the greatest start address at or below va (approximate: text and data follow it)
            lo, hi = 0, len(images)
            while lo < hi:
                mid = (lo + hi) // 2
                if images[mid][0] <= va:
                    lo = mid + 1
                else:
                    hi = mid
            name = images[lo - 1][1] if lo else '?'
            hits[name] = hits.get(name, 0) + 1
            i = mm.find(needle, i + 1, foff + size)
for name, n in sorted(hits.items()):
    print(n, name)
