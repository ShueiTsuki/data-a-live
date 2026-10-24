#!/usr/bin/env python3
"""The boot logo for shim/loader from an IPSW's applelogo image.

    python3 -I tools/bootlogo.py applelogo@3x~iphone.im4p logo.bin

On an iPhone, iBoot draws this logo before it starts the kernel, and it stays on the
screen until the system draws its first frame. shim/loader does the same with logo.bin.
The logo is Apple's: it comes from the user's own IPSW at build time and is never
committed (shim/loader/out is ignored).

Input: an IM4P (DER: "IM4P", type, version, payload) or its bare payload, an iBoot
image: "iBootIm\\0", adler32, compression "lzss" (stored reversed), format "argb" or
"grey" (reversed), width and height (16-bit), then LZSS data from offset 0x40.
"grey" pixels are two bytes, grey and 255 - alpha (applelogo: (255, 0) inside the apple, (255, 255)
around it, (x, 255 - x) on its edge); "argb" four, B G R A in memory.

Output (shim/loader/loader.c draws it centred on black): "PLOGO\\0\\0\\1", width and
height (32-bit little endian), then width * height 32-bit ARGB pixels, the alpha
already applied over black.
"""
import struct
import sys
import zlib


def der_payload(data):
    """The payload OCTET STRING of an IM4P, or DATA itself when it is not one."""
    if data[:1] != b'\x30':
        return data

    def length(buf, pos):
        n = buf[pos]
        if n < 0x80:
            return n, pos + 1
        k = n & 0x7f
        return int.from_bytes(buf[pos + 1:pos + 1 + k], 'big'), pos + 1 + k

    _, pos = length(data, 1)                 # SEQUENCE
    items = []
    while pos < len(data) and len(items) < 4:
        tag = data[pos]
        size, start = length(data, pos + 1)
        items.append((tag, data[start:start + size]))
        pos = start + size
    assert items[0] == (0x16, b'IM4P'), 'not an IM4P'
    assert items[3][0] == 0x04, 'no payload'
    return items[3][1]


def lzss(src, size):
    """Apple's LZSS (Okumura's: 4 KiB ring from N - F, 18-byte matches, spaces before)."""
    N, F, T = 4096, 18, 2
    ring = bytearray(b' ' * N)
    r, out, i, flags = N - F, bytearray(), 0, 0
    while i < len(src) and len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xff00
            i += 1
            if i >= len(src):
                break
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src):
                break
            pos = src[i] | (src[i + 1] & 0xf0) << 4
            n = (src[i + 1] & 0x0f) + T
            i += 2
            for k in range(n + 1):
                c = ring[(pos + k) & (N - 1)]
                out.append(c)
                ring[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out[:size])


def main():
    src, dst = sys.argv[1], sys.argv[2]
    img = der_payload(open(src, 'rb').read())
    assert img[:8] == b'iBootIm\0', 'not an iBoot image'
    adler, comp, fmt, w, h = struct.unpack_from('<I4s4sHH', img, 8)
    comp, fmt = comp[::-1], fmt[::-1]
    assert comp == b'lzss', comp
    bpp = {b'grey': 2, b'argb': 4}[fmt]
    pix = lzss(img[0x40:], w * h * bpp)
    assert len(pix) == w * h * bpp, ('short image', len(pix), w * h * bpp)
    out = bytearray(b'PLOGO\0\0\1' + struct.pack('<II', w, h))
    for p in range(w * h):
        if bpp == 2:
            g, a = pix[2 * p], 255 - pix[2 * p + 1]
            r = gg = b = g * a // 255
        else:
            b, gg, r, a = pix[4 * p:4 * p + 4]
            r, gg, b = r * a // 255, gg * a // 255, b * a // 255
        out += struct.pack('<I', 0xff000000 | r << 16 | gg << 8 | b)
    open(dst, 'wb').write(out)
    lit = sum(1 for p in range(w * h) if out[16 + 4 * p] or out[17 + 4 * p] or out[18 + 4 * p])
    clen = struct.unpack_from("<I", img, 0x1c)[0]   # the adler32 covers 0x0c up to the end of the LZSS data
    assert zlib.adler32(img[0x0c:0x40 + clen]) == adler, "adler32 mismatch: damaged image"
    print("%s: %ux%u %s, adler32 %08x ok, %u of %u pixels lit -> %s" % (src, w, h, fmt.decode(), adler, lit, w * h, dst))


if __name__ == '__main__':
    main()
