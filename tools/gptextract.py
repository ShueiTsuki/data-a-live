"""List the GPT of a raw LUN image and extract named partitions from it (read-only on the input).

    python gptextract.py LUN.img                  list partitions
    python gptextract.py LUN.img OUTDIR NAME...   copy NAME partitions to OUTDIR/NAME.img
"""
import os
import struct
import sys
import uuid


def read_gpt(path):
    with open(path, 'rb') as f:
        for bs in (4096, 512):
            f.seek(bs)
            hdr = f.read(92)
            if hdr[:8] == b'EFI PART':
                break
        else:
            raise SystemExit('no GPT header at LBA 1 (4096 or 512 byte blocks)')
        (_, _, _, _, _, _, _, _, _, _, entries_lba, n, esize, _) = struct.unpack('<8sIIIIQQQQ16sQIII', hdr)
        f.seek(entries_lba * bs)
        raw = f.read(n * esize)
    parts = []
    for i in range(n):
        e = raw[i * esize:(i + 1) * esize]
        if e[:16] == b'\0' * 16:
            continue
        first, last = struct.unpack('<QQ', e[32:48])
        name = e[56:128].decode('utf-16-le').rstrip('\0')
        parts.append((name, first, last, bs, str(uuid.UUID(bytes_le=e[:16]))))
    return parts


def main():
    img = sys.argv[1]
    parts = read_gpt(img)
    if len(sys.argv) == 2:
        for name, first, last, bs, t in parts:
            print(f'{name:24s} lba {first:>10d}..{last:<10d} {((last - first + 1) * bs) >> 10:>9d} KiB')
        return
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    want = set(sys.argv[3:])
    with open(img, 'rb') as f:
        for name, first, last, bs, t in parts:
            if name not in want:
                continue
            dst = os.path.join(out, name + '.img')
            assert os.path.abspath(dst).startswith(os.path.abspath(out) + os.sep)
            f.seek(first * bs)
            with open(dst, 'wb') as o:
                o.write(f.read((last - first + 1) * bs))
            print('extracted', name, (last - first + 1) * bs, 'bytes')
            want.discard(name)
    if want:
        print('not found:', ' '.join(sorted(want)))


if __name__ == '__main__':
    main()
