#!/usr/bin/env python3
"""Frame dump from the pinned log file (patch 0159, boot-arg pandora_fbdump=SECS) to PNGs.

  fbdump.py LOGFILE_RAW OUT_PREFIX

LOGFILE_RAW is the whole log file read raw (tools/ios_test.sh keeps it as device/logs/NAME.raw
while decoding). The dump starts after the log header block and the 4 MiB ring (block 1025):
a 4096-byte header "PANDFBDM" v1, then up to two 32-bit BGRA frames, each downscaled 3x:
"surface" (the first shown layer backboardd handed to AppleCLCD) and "panel" (the panel
memory, i.e. what is on screen). Writes OUT_PREFIX-<name>.png; prints nothing to convert when
the dump area holds no dump.
"""
import struct
import sys
import zlib

DUMP_OFFSET = 1025 * 4096


def png(path, width, height, rgb_rows):
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rgb_rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    src, prefix = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    head = data[DUMP_OFFSET:DUMP_OFFSET + 4096]
    if len(head) < 4096 or head[:8] != b'PANDFBDM':
        print('fbdump: no frame dump in', src)
        return
    version, seq, frames, scale, uptime_ms = struct.unpack_from('<IIIIQ', head, 8)
    assert version == 1 and frames <= 2, (version, frames)
    print(f'fbdump: dump {seq} at {uptime_ms / 1000:.1f} s uptime, {frames} frames, 1/{scale} scale')
    for i in range(frames):
        name, width, height, offset, fmt = struct.unpack_from('<16sIIII', head, 32 + 32 * i)
        name = name.split(b'\0')[0].decode()
        start = DUMP_OFFSET + offset
        pix = data[start:start + width * height * 4]
        assert len(pix) == width * height * 4, (name, len(pix))
        rows = []
        for y in range(height):
            row = pix[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3] = row[2::4]
            rgb[1::3] = row[1::4]
            rgb[2::3] = row[0::4]
            rows.append(bytes(rgb))
        out = f'{prefix}-{name}.png'
        png(out, width, height, rows)
        print(f'fbdump: {name} {width}x{height} format 0x{fmt:08x} -> {out}')


if __name__ == '__main__':
    main()
