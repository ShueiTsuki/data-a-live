"""Pack what `fastboot boot` starts into the boot menu's iOS slot.

    python -I slot.py LOADER OUT --extents IOSROOT.json IOSLOG.json ADSPFW.json
                      [--keep-args] [--label TEXT]

LOADER  shim/loader/out/loader-boot.img (or its kernel, shim/loader/out/Image): the loader
        with its XNU payload. Any build that boots with `fastboot boot` fits.
The three JSON files are `pinfile extents FILE --json` of iosroot, ioslog and adspfw: the
phone's now (update_ios.sh) or saved ones (device/extents). The payload's ADT must hold
exactly these extents, otherwise iOS would read and write other blocks: refused.

OUT is the slot: a 4 KiB header page (menu.c struct bm_slot, armed) and the Image, whose
payload has everyday boot-args: the test-only words in DROP removed (not with --keep-args).
"""
import hashlib
import json
import struct
import sys

DROP = ('pandora_reboot_after', 'pandora_fbdump')
BOOT_LINE_LENGTH = 608
PAGE = 4096
# ADT /chosen property per pinned file, in the order of the hashes in the header
PROPS = ('fuxi-root-extents', 'pandora-log-extents', 'pandora-fw-extents')


def adt_node(buf, off):
    """Apple device tree node at off -> ((props, children), end)."""
    nprops, nchildren = struct.unpack_from('<II', buf, off)
    off += 8
    props = {}
    for _ in range(nprops):
        name = buf[off:off + 32].split(b'\0')[0].decode()
        size = struct.unpack_from('<I', buf, off + 32)[0] & 0x7fffffff
        off += 36
        props[name] = buf[off:off + size]
        off += (size + 3) & ~3
    children = []
    for _ in range(nchildren):
        child, off = adt_node(buf, off)
        children.append(child)
    return (props, children), off


def extent_words(data):
    """tools/mkadt_pandora.py root_extent_props: u64 {file block, partition block, blocks}."""
    table = json.loads(data)
    words, pos = [], 0
    for logical, physical, length, _flags in table['extents']:
        if logical != pos or physical % 4096 or length % 4096 or not length:
            return None
        words += [logical // 4096, physical // 4096, length // 4096]
        pos += length
    return struct.pack(f'<{len(words)}Q', *words)


def bm_sum(data):
    """menu.c bm_sum: a += w, b += a over 8-byte LE words (mod 2^64); a ^ rotl(b, 32)."""
    n = len(data) // 8
    words = struct.unpack(f'<{n}Q', data)
    m = (1 << 64) - 1
    a = sum(words) & m
    b = sum((n - i) * w for i, w in enumerate(words)) & m
    return a ^ (((b << 32) | (b >> 32)) & m)


def loader_image(path):
    data = open(path, 'rb').read()
    if data[:8] == b'ANDROID!':
        ksize = struct.unpack_from('<I', data, 8)[0]
        assert struct.unpack_from('<I', data, 40)[0] in (3, 4), 'boot image header v3/v4 expected'
        data = data[PAGE:PAGE + ksize]
    assert data[56:60] == b'ARM\x64', 'not an arm64 Image'
    return bytearray(data)


def main():
    args = sys.argv[1:]
    keep = '--keep-args' in args
    args = [a for a in args if a != '--keep-args']
    label = ''
    if '--label' in args:
        i = args.index('--label')
        label = args[i + 1]
        del args[i:i + 2]
    i = args.index('--extents')
    jsons = args[i + 1:i + 4]
    del args[i:i + 4]
    loader_path, out = args

    img = loader_image(loader_path)
    # the payload: tools/mkpayload.py format, 4 KiB aligned inside the loader Image
    offs = [o for o in range(0, len(img), PAGE) if img[o:o + 8] == b'SWEETXNU']
    assert len(offs) == 1, f'{len(offs)} SWEETXNU payloads in the Image'
    pl = offs[0]
    assert struct.unpack_from('<I', img, pl + 8)[0] == 1, 'payload version'
    adt_off, adt_size = struct.unpack_from('<QQ', img, pl + 32)
    c = pl + 72
    words = img[c:c + BOOT_LINE_LENGTH].split(b'\0')[0].decode().split()
    if not keep:
        words = [w for w in words if w.split('=')[0] not in DROP]
    new = ' '.join(words).encode()
    assert len(new) < BOOT_LINE_LENGTH
    img[c:c + BOOT_LINE_LENGTH] = new + b'\0' * (BOOT_LINE_LENGTH - len(new))

    (_, children), _ = adt_node(bytes(img[pl + adt_off:pl + adt_off + adt_size]), 0)
    chosen = [ch for ch in children if ch[0].get('name', b'').rstrip(b'\0') == b'chosen']
    assert len(chosen) == 1, 'no /chosen in the ADT'
    digests = b''
    for prop, path in zip(PROPS, jsons):
        data = open(path, 'rb').read()
        want = chosen[0][0].get(prop)
        assert want, f'/chosen/{prop} missing: the payload was built without that pinned file'
        assert extent_words(data) == want, f'{path} does not hold the extents in /chosen/{prop}'
        digests += hashlib.sha256(data).digest()
        print(f'{prop}: {len(want) // 24} extents = {path}')

    if len(img) % 8:
        img += b'\0' * (-len(img) % 8)
    hdr = bytearray(PAGE)
    hdr[0:8] = b'PNDSLOT1'
    struct.pack_into('<IIQQ', hdr, 8, 1, 0, len(img), bm_sum(bytes(img)))
    hdr[32:128] = digests
    hdr[192:256] = label.encode()[:63].ljust(64, b'\0')
    open(out, 'wb').write(bytes(hdr) + bytes(img))
    print('boot-args:', new.decode())
    print(f'slot {out}: {PAGE + len(img)} bytes (Image {len(img)}), sum {struct.unpack_from("<Q", hdr, 24)[0]:#018x}')


if __name__ == '__main__':
    main()
