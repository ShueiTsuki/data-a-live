"""Pack the boot menu Image into a full boot_b partition image.

    python -I mkboot.py OUTDIR STOCK_BOOT [--wait-ms N] [--any-mode] [--unarmed]

OUTDIR holds Image (build.sh), payload.bin, linux.Image and extents.txt (prep.py); writes
OUTDIR/boot-menu.img (as large as the stock partition) and OUTDIR/layout.txt.

The boot image header is the stock one with only kernel_size changed (os_version 0, empty
cmdline, no ramdisk, as stock). The stock AVB vbmeta blob is kept byte for byte behind the
new contents with a new footer, so ABL reads the same boot os_version and security_patch;
its hash no longer matches, which the unlocked bootloader accepts (orange state, as now).
"""
import hashlib
import os
import struct
import sys

PAGE = 4096
SZ_2M = 0x200000
SAVE_AREA = 0x100000      # menu.c BAND_BYTES (4880 * 96) rounded up
MAGIC = b'PNDBMCF1'


def main():
    args = sys.argv[1:]
    def opt(name, default=None):
        if name in args:
            i = args.index(name)
            v = args[i + 1]
            del args[i:i + 2]
            return v
        return default
    wait_ms = int(opt('--wait-ms', '5000'))
    any_mode = '--any-mode' in args
    unarmed = '--unarmed' in args
    args = [a for a in args if a not in ('--any-mode', '--unarmed')]
    out, stock_path = args
    p = lambda n: os.path.join(out, n)

    img = bytearray(open(p('Image'), 'rb').read())
    payload = open(p('payload.bin'), 'rb').read()
    linux = open(p('linux.Image'), 'rb').read()
    stock = open(stock_path, 'rb').read()
    assert img[56:60] == b'ARM\x64', 'not an arm64 Image'
    assert 1000 <= wait_ms <= 9000, 'wait between 1 and 9 s (one digit on screen)'

    pl_off = img.find(payload[:64])
    assert pl_off > 0 and pl_off % PAGE == 0 and img[pl_off:pl_off + len(payload)] == payload, 'payload'
    lx_off = img.find(linux[:64], pl_off + len(payload))
    assert lx_off % SZ_2M == 0 and img[lx_off:] == linux, 'Android kernel not at the 2 MiB aligned end'
    assert struct.unpack_from('<Q', img, 16)[0] == len(img)

    # image_size: the Android kernel's own image_size (BSS), the move slack, the save area
    lx_size = struct.unpack_from('<Q', linux, 16)[0]
    declared = lx_off + lx_size + SZ_2M + SAVE_AREA
    struct.pack_into('<Q', img, 16, declared)

    # settings page
    hits = [i for i in range(0, pl_off, PAGE) if img[i:i + 8] == MAGIC]
    assert len(hits) == 1, f'settings page found {len(hits)} times'
    cfg = hits[0]
    digests = {}
    for ln in open(p('extents.txt')):
        name, digest, _json = ln.split()
        digests[name] = bytes.fromhex(digest)
    sha = b''.join(digests[n] for n in ('iosroot', 'ioslog', 'adspfw'))
    struct.pack_into('<IIII', img, cfg + 8, 0 if unarmed else 1, wait_ms, int(any_mode), 0)
    img[cfg + 24:cfg + 24 + 96] = sha
    img[cfg + 120:cfg + 184] = (b'not armed at build' if unarmed else b'').ljust(64, b'\0')

    # header: stock, kernel_size changed
    assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 4
    ramdisk, os_version, hdr_size = struct.unpack_from('<III', stock, 12)
    assert (ramdisk, os_version, hdr_size) == (0, 0, 1584), (ramdisk, os_version, hdr_size)
    hdr = bytearray(stock[:PAGE])
    struct.pack_into('<I', hdr, 8, len(img))
    body = bytes(hdr) + bytes(img) + b'\0' * (-len(img) % PAGE)

    # AVB: stock vbmeta blob, new footer
    footer = stock[-64:]
    assert footer[:4] == b'AVBf'
    _, _, _, vb_off, vb_size = struct.unpack('>IIQQQ', footer[4:36])
    vbmeta = stock[vb_off:vb_off + vb_size]
    part = len(stock)
    room = part - PAGE - (len(body) + len(vbmeta))
    assert room >= 0, f'{-room} bytes too large for the {part} byte partition'
    new_footer = b'AVBf' + struct.pack('>IIQQQ', 1, 0, len(body), len(body), len(vbmeta)) + b'\0' * 28
    assert len(new_footer) == 64
    image = body + vbmeta
    image += b'\0' * (part - 64 - len(image)) + new_footer
    assert len(image) == part
    open(p('boot-menu.img'), 'wb').write(image)

    digest = hashlib.sha256(image).hexdigest()
    info = [
        f'boot-menu.img {part} bytes sha256 {digest}',
        f'kernel field {len(img)} bytes (menu + payload at {pl_off:#x} + Android kernel at {lx_off:#x}),'
        f' image_size {declared:#x}',
        f'settings page at partition offset {PAGE + cfg:#x} (armed {0 if unarmed else 1},'
        f' wait {wait_ms} ms, any mode {int(any_mode)})',
        f'free space before the vbmeta/footer page: {room} bytes ({room / 2**20:.1f} MiB)',
    ]
    open(p('layout.txt'), 'w', newline='\n').write('\n'.join(info) + '\n')
    print('\n'.join(info))


if __name__ == '__main__':
    main()
