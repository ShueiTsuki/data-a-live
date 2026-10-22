#!/usr/bin/env python3
"""Pack firmware files into one image for a pinned userdata file (kernel: pandora_adsp.c).

  mkfwpack.py OUT.img FILE...

Block 0 (4 KiB): "PANDFW01" | u32 count | u32 reserved | count x {char name[48]; u64 offset;
u64 size} (64 bytes each, at most 62). Files follow, each starting on a 4 KiB boundary.
"""
import os
import struct
import sys

out, files = sys.argv[1], sys.argv[2:]
assert 0 < len(files) <= 62, 'at most 62 files'
entries, blobs, off = [], [], 4096
for f in files:
    data = open(f, 'rb').read()
    name = os.path.basename(f).encode()
    assert len(name) < 48
    entries.append(struct.pack('<48sQQ', name, off, len(data)))
    blobs.append(data + b'\0' * (-len(data) % 4096))
    off += len(blobs[-1])
head = b'PANDFW01' + struct.pack('<II', len(files), 0) + b''.join(entries)
head += b'\0' * (4096 - len(head))
with open(out, 'wb') as o:
    o.write(head)
    for b in blobs:
        o.write(b)
print(out, len(files), 'files', off, 'bytes')
