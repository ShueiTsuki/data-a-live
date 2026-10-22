#!/usr/bin/env python3
"""Decode the kernel's console log file (patch 0129) read with `pinfile rawread`.

  pandora_log.py RAW [OUT.txt]

Block 0: "PANDORLG" | u32 version | u32 ring size | u64 total bytes | u64 flush seq.
Blocks 1..: the ring; byte n of the console stream is at ring offset n % size.
"""
import struct
import sys


def decode(raw):
    if raw[:8] != b'PANDORLG':
        raise SystemExit('no PANDORLG header (log never flushed?)')
    version, size, total, seq = struct.unpack_from('<IIQQ', raw, 8)
    ring = raw[4096:4096 + size]
    if len(ring) < size:
        raise SystemExit(f'file shorter than the ring ({len(ring)} < {size})')
    if total <= size:
        text = ring[:total]
    else:
        start = total % size
        text = ring[start:] + ring[:start]
    return version, size, total, seq, text


def main():
    raw = open(sys.argv[1], 'rb').read()
    version, size, total, seq, text = decode(raw)
    out = sys.argv[2] if len(sys.argv) > 2 else None
    head = f'# version {version}, ring {size}, {total} bytes written, flush {seq}' + \
        (f', oldest {total - size} bytes lost' if total > size else '') + '\n'
    data = head.encode() + text
    if out:
        open(out, 'wb').write(data)
        print(head.strip(), '->', out)
    else:
        sys.stdout.buffer.write(data)


if __name__ == '__main__':
    main()
