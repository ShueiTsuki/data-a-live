"""Pack the boot menu into a full boot_b partition image.

    python -I mkboot.py OUTDIR STOCK_BOOT [--slot SLOT] [--wait-ms N] [--any-mode]

OUTDIR holds Image (build.sh: the menu with the stock Android kernel 2 MiB in) and
linux.Image (prep.py). SLOT (slot.py) goes into the iOS slot; without it the slot is empty
and the menu shows "iOS off". Writes OUTDIR/boot-menu.img (as large as the stock partition)
and OUTDIR/layout.txt.

Layout of the kernel field, the same for every later iOS: menu | settings page | Android
kernel | iOS slot up to the end of the field. kernel_size covers the whole field, so the
boot image header never changes when update_ios.sh replaces the slot. The header is the
stock one with only kernel_size changed (os_version 0, empty cmdline, no ramdisk). The stock
AVB vbmeta blob is kept byte for byte behind the field with a new footer, so ABL reads the
same boot os_version and security_patch; its hash no longer matches, which the unlocked
bootloader accepts (orange state, as now).
"""
import hashlib
import os
import struct
import sys

PAGE = 4096
SZ_2M = 0x200000
SLOT_ALIGN = 0x10000
SAVE_AREA = 0x100000      # menu.c BAND_BYTES (4880 * 96) rounded up
MAGIC = b'PNDBMCF2'


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
    slot_path = opt('--slot')
    any_mode = '--any-mode' in args
    args = [a for a in args if a != '--any-mode']
    out, stock_path = args
    p = lambda n: os.path.join(out, n)

    img = bytearray(open(p('Image'), 'rb').read())
    linux = open(p('linux.Image'), 'rb').read()
    stock = open(stock_path, 'rb').read()
    assert img[56:60] == b'ARM\x64', 'not an arm64 Image'
    assert 1000 <= wait_ms <= 9000, 'wait between 1 and 9 s (one digit on screen)'
    assert img[SZ_2M:] == linux, 'Android kernel not at 2 MiB'
    assert struct.unpack_from('<Q', img, 16)[0] == len(img)

    # the field: menu + Android kernel, the slot, the stock vbmeta behind it, the footer page
    footer = stock[-64:]
    assert footer[:4] == b'AVBf'
    _, _, _, vb_off, vb_size = struct.unpack('>IIQQQ', footer[4:36])
    vbmeta = stock[vb_off:vb_off + vb_size]
    part = len(stock)
    field = part - PAGE - (vb_size + PAGE - 1) // PAGE * PAGE - PAGE
    slot_off = (len(img) + SLOT_ALIGN - 1) // SLOT_ALIGN * SLOT_ALIGN
    slot_max = field - slot_off
    slot = open(slot_path, 'rb').read() if slot_path else b''
    assert len(slot) <= slot_max, f'slot {len(slot)} bytes, room for {slot_max}'
    assert not slot or slot[:8] == b'PNDSLOT1', 'not a slot (slot.py)'

    # image_size: the field, the Android kernel's own image_size (BSS), the save area
    lx_size = struct.unpack_from('<Q', linux, 16)[0]
    save_off = max(field, SZ_2M + lx_size + SZ_2M)
    declared = save_off + SAVE_AREA
    struct.pack_into('<Q', img, 16, declared)

    # settings page
    hits = [i for i in range(0, SZ_2M, PAGE) if img[i:i + 8] == MAGIC]
    assert len(hits) == 1, f'settings page found {len(hits)} times'
    cfg = hits[0]
    struct.pack_into('<IIQQQ', img, cfg + 8, wait_ms, int(any_mode), slot_off, slot_max, save_off)

    kernel = bytes(img) + b'\0' * (slot_off - len(img)) + slot + b'\0' * (slot_max - len(slot))
    assert len(kernel) == field

    # header: stock, kernel_size changed
    assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 4
    ramdisk, os_version, hdr_size = struct.unpack_from('<III', stock, 12)
    assert (ramdisk, os_version, hdr_size) == (0, 0, 1584), (ramdisk, os_version, hdr_size)
    hdr = bytearray(stock[:PAGE])
    struct.pack_into('<I', hdr, 8, field)
    body = bytes(hdr) + kernel
    new_footer = b'AVBf' + struct.pack('>IIQQQ', 1, 0, len(body), len(body), len(vbmeta)) + b'\0' * 28
    image = body + vbmeta
    image += b'\0' * (part - 64 - len(image)) + new_footer
    assert len(image) == part
    open(p('boot-menu.img'), 'wb').write(image)

    digest = hashlib.sha256(image).hexdigest()
    info = [
        f'boot-menu.img {part} bytes sha256 {digest}',
        f'kernel field {field} bytes: menu, Android kernel at {SZ_2M:#x}, image_size {declared:#x}',
        f'settings page at partition offset {PAGE + cfg:#x} (wait {wait_ms} ms, any mode {int(any_mode)})',
        f'iOS slot at partition offset {PAGE + slot_off:#x}, {slot_max} bytes ({slot_max / 2**20:.1f} MiB);'
        f' holds {len(slot)} bytes ({(slot_max - len(slot)) / 2**20:.1f} MiB free)',
    ]
    open(p('layout.txt'), 'w', newline='\n').write('\n'.join(info) + '\n')
    print('\n'.join(info))


if __name__ == '__main__':
    main()
