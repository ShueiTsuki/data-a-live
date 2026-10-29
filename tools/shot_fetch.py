#!/usr/bin/env python3
"""Screen shots of the last iOS boot, as PNGs.

  shot_fetch.py OUTDIR

Patch 0308: volume up and volume down held together save what is on screen as /private/var/tmp/pandora_shot_N.bgra
(bsd/dev/pandora_files.c: a 64-byte header "PANDSHOT", version 1, width, height, N, uptime in ms; then BGRA pixels,
tightly packed). With the phone in Android this reads them out of the iOS root image the way tools/ioslog_fetch.py
reads the logs (pinfile rawreadat, read-only) and writes OUTDIR/pandora_shot_N_HHMMSS.png, HHMMSS the file's
modification time (UTC), so shots of different boots with the same N stay apart. Device data: OUTDIR outside the repo.
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ioslog_fetch import HFS, HFS_EPOCH, Remote  # noqa: E402


def png(path, width, height, rows):
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    fs = HFS(Remote())
    ents = fs.walk()
    kids = {}
    for (parent, name), v in ents.items():
        kids.setdefault(parent, {})[name] = v
    cur = 2                                        # the root folder's CNID
    for part in ('private', 'var', 'tmp'):
        v = kids.get(cur, {}).get(part)
        if v is None or v[0] != 1:
            sys.exit(f'no folder /private/var/tmp ({part})')
        cur = v[1]
    shots = sorted((name, v) for name, v in kids.get(cur, {}).items()
                   if v[0] == 2 and name.startswith('pandora_shot_') and name.endswith('.bgra'))
    if not shots:
        print('no shots in /private/var/tmp')
    for name, v in shots:
        data = fs.read_fork(v[3][0], v[1])
        magic, version, w, h, seq, uptime = struct.unpack_from('<8sIIIIQ', data)
        if magic != b'PANDSHOT' or version != 1 or len(data) < 64 + w * h * 4:
            print(f'{name}: not a shot ({len(data)} bytes)')
            continue
        px = memoryview(data)[64:64 + w * h * 4]
        rows = []
        for y in range(h):
            r = bytearray(px[y * w * 4:(y + 1) * w * 4])
            rgb = bytearray(w * 3)
            rgb[0::3] = r[2::4]
            rgb[1::3] = r[1::4]
            rgb[2::3] = r[0::4]
            rows.append(bytes(rgb))
        stamp = time.strftime('%H%M%S', time.gmtime(v[2] - HFS_EPOCH))
        dst = os.path.join(out, f'{name[:-5]}_{stamp}.png')
        assert os.path.dirname(os.path.abspath(dst)) == os.path.abspath(out)
        png(dst, w, h, rows)
        print(f'{dst}: {w}x{h}, shot {seq} at {uptime / 1000:.1f} s after boot')


if __name__ == '__main__':
    main()
