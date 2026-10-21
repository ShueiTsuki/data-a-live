"""Describe Android boot / init_boot / vendor_boot images (header v3/v4) and the kernel inside.

    python bootimg_info.py boot_b.img [vendor_boot_b.img ...]
Read-only. Prints header fields, payload offsets, the kernel's format and its arm64 Image header.
"""
import gzip
import struct
import sys


def avb_footer(data):
    f = data[-64:]
    if f[:4] == b'AVBf':
        major, minor, orig, vb_off, vb_size = struct.unpack('>4xII QQQ', f[:36])
        return f'AVB footer: original image size {orig}, vbmeta at {vb_off} ({vb_size} bytes)'
    return 'no AVB footer'


def pages(n, page):
    return (n + page - 1) // page * page


def describe_kernel(k):
    kind = 'unknown'
    raw = None
    if k[:2] == b'\x1f\x8b':
        kind, raw = 'gzip', gzip.decompress(k)
    elif k[:4] == b'\x02\x21\x4c\x18':
        kind = 'lz4 legacy (not decompressed here)'
    elif k[0x38:0x3c] == b'ARM\x64':
        kind, raw = 'raw arm64 Image', k
    print(f'  kernel format: {kind}, {len(k)} bytes')
    if raw and raw[0x38:0x3c] == b'ARM\x64':
        code0, code1, text_offset, image_size, flags = struct.unpack('<IIQQQ', raw[:32])
        page = {0: 'unspecified', 1: '4K', 2: '16K', 3: '64K'}[(flags >> 1) & 3]
        print(f'  arm64 Image header: text_offset 0x{text_offset:x}, image_size 0x{image_size:x} '
              f'({image_size >> 20} MiB), flags 0x{flags:x} (page size {page}, '
              f'{"anywhere in RAM" if flags & 8 else "2 MiB aligned near RAM base"}, '
              f'{"big" if flags & 1 else "little"}-endian)')
        i = raw.find(b'Linux version ')
        if i >= 0:
            print('  ' + raw[i:raw.index(b'\n', i)].decode(errors='replace'))


def boot(data):
    (magic, ksize, rsize, osver, hsize) = struct.unpack('<8sIII I', data[:24])[:5]
    hdr_ver = struct.unpack('<I', data[40:44])[0]
    print(f'  boot image header v{hdr_ver}, header {hsize} bytes, page 4096 (fixed for v3/v4)')
    print(f'  kernel {ksize} bytes, ramdisk {rsize} bytes, os_version 0x{osver:x}')
    cmd = data[44:44 + 1536].split(b'\0')[0].decode(errors='replace')
    print(f'  cmdline: "{cmd}"')
    if hdr_ver >= 4:
        sig = struct.unpack('<I', data[44 + 1536:44 + 1540])[0]
        print(f'  boot signature size {sig}')
    page = 4096
    koff = pages(hsize, page)
    print(f'  kernel at offset 0x{koff:x}, ramdisk at 0x{koff + pages(ksize, page):x}')
    if ksize:
        describe_kernel(data[koff:koff + ksize])


def vendor_boot(data):
    magic, ver, page, kaddr, raddr, vrsize = struct.unpack('<8sIIIII', data[:28])
    cmd = data[28:28 + 2048].split(b'\0')[0].decode(errors='replace')
    o = 28 + 2048
    tags, name = struct.unpack('<I16s', data[o:o + 20])
    hsize, dtbsize, dtbaddr = struct.unpack('<IIQ', data[o + 20:o + 36])
    print(f'  vendor_boot header v{ver}, page {page}, header {hsize} bytes')
    print(f'  load addresses: kernel 0x{kaddr:x}, ramdisk 0x{raddr:x}, tags 0x{tags:x}, dtb 0x{dtbaddr:x}')
    print(f'  board name "{name.split(b"\0")[0].decode()}", vendor ramdisk {vrsize} bytes, dtb {dtbsize} bytes')
    print(f'  cmdline: "{cmd}"')
    if ver >= 4:
        tsize, tnum, tesize, bcsize = struct.unpack('<IIII', data[o + 36:o + 52])
        print(f'  ramdisk table {tnum} entries, bootconfig {bcsize} bytes')
    vr_off = pages(hsize, page)
    dtb_off = vr_off + pages(vrsize, page)
    dtb = data[dtb_off:dtb_off + dtbsize]
    print(f'  dtb at offset 0x{dtb_off:x}, starts with {dtb[:4].hex()} '
          f'({"FDT" if dtb[:4] == bytes.fromhex("d00dfeed") else "not a plain FDT"})')
    return dtb


def main():
    for path in sys.argv[1:]:
        data = open(path, 'rb').read()
        print(path)
        if data[:8] == b'ANDROID!':
            boot(data)
        elif data[:8] == b'VNDRBOOT':
            dtb = vendor_boot(data)
            if dtb[:4] == bytes.fromhex('d00dfeed'):
                out = path.rsplit('.', 1)[0] + '.dtb'
                open(out, 'wb').write(dtb)
                print(f'  dtb blob saved to {out}')
        else:
            print(f'  unknown magic {data[:8]!r}')
        print('  ' + avb_footer(data))


if __name__ == '__main__':
    main()
