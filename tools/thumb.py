#!/usr/bin/env python3
"""Render the screen thumbnails that patch 0142 writes into the kernel log.

  thumb.py LOG.txt OUTDIR      -> OUTDIR/<tag>.png for every "SWEET THUMB <tag> <row> <hex>"

Each sample is one hex digit of grey; images are scaled up 6x so they can be looked at.
"""
import os
import re
import sys

from PIL import Image

log, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
thumbs = {}
for line in open(log, 'rb').read().decode('latin-1').splitlines():
    m = re.search(r'SWEET THUMB (\S+) (\d{3}) ([0-9a-f]+)', line)
    if m:
        thumbs.setdefault(m.group(1), {})[int(m.group(2))] = m.group(3)
for tag, rows in thumbs.items():
    w = max(len(r) for r in rows.values())
    h = max(rows) + 1
    img = Image.new('L', (w, h))
    for y in range(h):
        r = rows.get(y, '')
        for x, ch in enumerate(r):
            img.putpixel((x, y), int(ch, 16) * 17)
    img = img.resize((w * 6, h * 6), Image.NEAREST)
    path = os.path.join(out, tag + '.png')
    img.save(path)
    print(path, f'{w}x{h}', f'{len(rows)} rows')
