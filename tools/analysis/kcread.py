"""kcread.py MACHO VADDR COUNT: COUNT little-endian int32 words at VADDR of a Mach-O (its LC_SEGMENT_64 mappings)."""
import struct
import sys

path, va, count = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
data = open(path, 'rb').read()
ncmds = struct.unpack_from('<I', data, 16)[0]
off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:
        name = data[off + 8:off + 24].rstrip(b'\0').decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off + 24)
        if vmaddr <= va < vmaddr + filesize:
            p = fileoff + va - vmaddr
            words = struct.unpack_from('<%di' % count, data, p)
            print(name, ' '.join('%d' % w for w in words))
            sys.exit(0)
    off += size
sys.exit('not mapped')
