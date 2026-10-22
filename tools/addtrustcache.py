#!/usr/bin/env python3
"""Add trust caches to a SWEETXNU payload (ios-sweet tools/mkpayload.py output), as iBoot would.

  addtrustcache.py PAYLOAD OUT TRUSTCACHE...

Each TRUSTCACHE is an IPSW trust cache (IM4P "trst", e.g. Firmware/<OS dmg>.trustcache, the
StaticTrustCache of BuildManifest.plist) or its raw payload. The first one becomes the static
trust cache (bsd/kern/kern_trustcache.c load_static_trust_cache: module 0 = kTCTypeStatic).

The caches are packed the way the kernel reads them (bsd/sys/trust_caches.h):
  u32 num_caches; u32 offsets[num_caches] (from the start of this block); caches...
and the block becomes one more payload segment right after kernel_end, like mkpayload's
--ramdisk: the loader copies it below the ADT and boot_args, inside topOfKernelData, so the
VM never gets that memory. The ADT gets /chosen/memory-map "TrustCache" = <phys, length>
(DTTrustCacheRange: vm_offset_t paddr, size_t length). Needs ios-sweet's adt.py next to it.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

PAGE = 0x4000
SEG_TABLE = 0x2a8


def raw_trust_cache(path):
    buf = open(path, 'rb').read()
    if buf[:1] == b'\x30' and b'IM4P' in buf[:16]:
        buf, info = adt.im4p_payload(buf)
        assert info['type'] in ('trst', 'trcs') and not info['encrypted'], (path, info)
    version, = struct.unpack_from('<I', buf, 0)
    assert version == 2, f'{path}: trust cache version {version}, expected 2'
    count, = struct.unpack_from('<I', buf, 20)
    assert len(buf) == 24 + 24 * count, f'{path}: {len(buf)} bytes for {count} entries'
    hashes = [buf[24 + 24 * i:24 + 24 * i + 20] for i in range(count)]
    assert hashes == sorted(hashes), f'{path}: entries not sorted'
    return buf, count


def bundle(caches):
    head = 4 + 4 * len(caches)
    offsets, body, at = [], b'', (head + 15) & ~15
    for c in caches:
        offsets.append(at + len(body))
        body += c + b'\0' * (-len(c) & 15)
    blob = struct.pack(f'<I{len(caches)}I', len(caches), *offsets)
    return blob + b'\0' * (((head + 15) & ~15) - head) + body


def main():
    src, out, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    assert paths, 'no trust caches given'
    p = open(src, 'rb').read()
    magic, version, nsegs = struct.unpack_from('<8sII', p, 0)
    assert magic == b'SWEETXNU' and version == 1, 'not a SWEETXNU v1 payload'
    virt_base, entry, adt_off, adt_size, kernel_end, phys_base, mem_size = struct.unpack_from('<7Q', p, 0x10)
    cmdline = p[0x48:SEG_TABLE]
    segs = [struct.unpack_from('<4Q', p, SEG_TABLE + 32 * i) for i in range(nsegs)]
    pieces = [(va, size, p[off:off + n]) for va, size, off, n in segs]
    caches = []
    for path in paths:
        tc, count = raw_trust_cache(path)
        caches.append(tc)
        print(f'{os.path.basename(path)}: {count} entries, uuid {tc[4:20].hex()}')
    blob = bundle(caches)
    tc_va = kernel_end
    tc_size = (len(blob) + PAGE - 1) & ~(PAGE - 1)
    tc_pa = tc_va - virt_base + phys_base
    pieces.append((tc_va, tc_size, blob))
    kernel_end = tc_va + tc_size

    root, _ = adt.parse(p[adt_off:adt_off + adt_size])
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next(c for c in chosen.children if c.name == 'memory-map')
    assert mmap.prop('TrustCache') is None, 'ADT already has a TrustCache range'
    mmap.props.append(('TrustCache', 0, struct.pack('<2Q', tc_pa, len(blob))))
    adt_blob = adt.serialize(root)

    o = bytearray(SEG_TABLE + 32 * len(pieces))
    table = []
    for va, size, data in pieces:
        off = len(o)
        o += data
        o += b'\0' * (-len(o) & 0xf)
        table.append((va, size, off, len(data)))
    adt_off = len(o)
    o += adt_blob
    o += b'\0' * (-len(o) & 0xf)
    struct.pack_into('<8sII', o, 0, b'SWEETXNU', 1, len(pieces))
    struct.pack_into('<7Q', o, 0x10, virt_base, entry, adt_off, len(adt_blob), kernel_end, phys_base, mem_size)
    o[0x48:SEG_TABLE] = cmdline
    for i, t in enumerate(table):
        struct.pack_into('<4Q', o, SEG_TABLE + 32 * i, *t)
    open(out, 'wb').write(o)
    print(f'trust caches: {len(caches)} modules, {len(blob)} bytes at phys {tc_pa:#x} '
          f'(va {tc_va:#x}); kernel_end {kernel_end:#x}; payload {len(o)} bytes')


if __name__ == '__main__':
    main()
