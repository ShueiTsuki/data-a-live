#!/usr/bin/env python3
"""Trust cache (version 2, raw payload) with the CodeDirectory hash of each given Mach-O.

  mktc.py OUT BINARY...

The cdhash is the first 20 bytes of the hash of the CodeDirectory blob in LC_CODE_SIGNATURE,
with the CodeDirectory's own hash type (2 = SHA-256). Entries are cdhash, hash type, flags 0,
constraint category 0, reserved 0, sorted, as tools/addtrustcache.py expects; pass the result
with '+' to merge it into a static cache module, e.g. TRUSTCACHE="A B+C+OUT".
"""
import hashlib
import struct
import sys

LC_CODE_SIGNATURE = 0x1d
CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02
HASHES = {1: hashlib.sha1, 2: hashlib.sha256}


def cdhash(path):
    b = open(path, 'rb').read()
    magic, _, _, _, ncmds = struct.unpack_from('<5I', b, 0)
    assert magic == 0xfeedfacf, f'{path}: not a 64-bit Mach-O'
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', b, at)
        if cmd == LC_CODE_SIGNATURE:
            off, n = struct.unpack_from('<2I', b, at + 8)
            sb = b[off:off + n]
            m, _, count = struct.unpack_from('>3I', sb, 0)
            assert m == CSMAGIC_EMBEDDED_SIGNATURE, f'{path}: no embedded signature'
            # The CodeDirectory (slot 0) and alternates (0x1000..): codesign may write a SHA-1
            # one in slot 0 and a SHA-256 one as an alternate; the kernel takes the best hash.
            best = None
            for i in range(count):
                typ, boff = struct.unpack_from('>2I', sb, 12 + 8 * i)
                if typ == 0 or 0x1000 <= typ < 0x1005:
                    cm, clen = struct.unpack_from('>2I', sb, boff)
                    assert cm == CSMAGIC_CODEDIRECTORY
                    cd = sb[boff:boff + clen]
                    ht = cd[37]
                    if best is None or ht > best[1]:
                        best = (HASHES[ht](cd).digest()[:20], ht)
            if best:
                return best
        at += size
    raise SystemExit(f'{path}: not signed')


def main():
    out, bins = sys.argv[1], sys.argv[2:]
    entries = []
    for p in bins:
        h, ht = cdhash(p)
        print(f'{p}: cdhash {h.hex()} (hash type {ht})')
        entries.append(h + bytes([ht, 0, 0, 0]))
    entries.sort()
    uuid = hashlib.sha256(b''.join(entries)).digest()[:16]
    open(out, 'wb').write(struct.pack('<I16sI', 2, uuid, len(entries)) + b''.join(entries))


if __name__ == '__main__':
    main()
