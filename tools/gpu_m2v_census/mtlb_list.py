"""List entry functions of a metallib (MTLB): name and type (0 vertex, 1 fragment, 2 kernel)."""
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:4] == b'MTLB'
# header: magic, u16 platform, u16 version..., then u64 file size, function list offset/size ...
flo, fls = struct.unpack_from('<QQ', b, 0x18)
n, = struct.unpack_from('<I', b, flo)
off = flo + 4
types = {0: 'vertex', 1: 'fragment', 2: 'kernel'}
for i in range(n):
    size, = struct.unpack_from('<I', b, off)
    end = off + size
    p = off + 4
    name, ftype = None, None
    while p < end:
        tag = b[p:p + 4]
        if tag == b'ENDT':
            break
        ln, = struct.unpack_from('<H', b, p + 4)
        val = b[p + 6:p + 6 + ln]
        if tag == b'NAME':
            name = val.rstrip(b'\0').decode()
        elif tag == b'TYPE':
            ftype = val[0]
        p += 6 + ln
    print(types.get(ftype, ftype), name)
    off = end
