"""Inputs for the boot menu image.

    python -I prep.py PAYLOAD STOCK_BOOT OUTDIR [--extents-dir DIR] [--keep-args]

PAYLOAD     the XNU payload (shim/loader/out/payload.bin, tools/mkpayload.py format)
STOCK_BOOT  the phone's stock boot_b (a full 96 MiB partition copy)

Writes to OUTDIR:
  payload.bin       the payload with everyday boot-args: the test-only words in DROP removed
  linux.Image       the stock Android kernel, byte for byte
  stock_vbmeta.bin  the stock AVB vbmeta blob (boot os_version / security_patch props)
  extents.txt       for iosroot, ioslog, adspfw: the saved pinfile JSON whose extents equal
                    the ones baked into the payload's ADT, and its sha256 (the watchdog
                    compares `pinfile extents FILE --json` on the phone with it)
"""
import glob
import hashlib
import json
import os
import struct
import sys

DROP = ('pandora_reboot_after', 'pandora_fbdump')
BOOT_LINE_LENGTH = 608
# ADT /chosen property -> saved JSON prefix in device/extents (tools/boot_ios.sh names)
FILES = (('fuxi-root-extents', 'iosroot'), ('pandora-log-extents', 'ioslog'),
         ('pandora-fw-extents', 'adspfw'))


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


def extent_words(path):
    """tools/mkadt_pandora.py root_extent_props: u64 {file block, partition block, blocks}."""
    table = json.load(open(path))
    words, pos = [], 0
    for logical, physical, length, _flags in table['extents']:
        if logical != pos or physical % 4096 or length % 4096 or not length:
            return None
        words += [logical // 4096, physical // 4096, length // 4096]
        pos += length
    return struct.pack(f'<{len(words)}Q', *words)


def main():
    args = sys.argv[1:]
    keep = '--keep-args' in args
    ext_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'device', 'extents')
    if '--extents-dir' in args:
        i = args.index('--extents-dir')
        ext_dir = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if a != '--keep-args']
    payload_path, stock_path, out = args
    os.makedirs(out, exist_ok=True)

    # payload: everyday boot-args
    pl = bytearray(open(payload_path, 'rb').read())
    assert pl[:8] == b'SWEETXNU' and struct.unpack_from('<I', pl, 8)[0] == 1, 'not a SWEETXNU v1 payload'
    adt_off, adt_size = struct.unpack_from('<QQ', pl, 32)
    cmd = pl[72:72 + BOOT_LINE_LENGTH].split(b'\0')[0].decode()
    words = cmd.split()
    if not keep:
        words = [w for w in words if w.split('=')[0] not in DROP]
    new = ' '.join(words).encode()
    assert len(new) < BOOT_LINE_LENGTH
    pl[72:72 + BOOT_LINE_LENGTH] = new + b'\0' * (BOOT_LINE_LENGTH - len(new))
    open(os.path.join(out, 'payload.bin'), 'wb').write(pl)
    print('boot-args:', new.decode())

    # extents baked into the ADT -> the saved JSON with the same extents
    (_, children), _ = adt_node(pl[adt_off:adt_off + adt_size], 0)
    chosen = [c for c in children if c[0].get('name', b'').rstrip(b'\0') == b'chosen']
    assert len(chosen) == 1, 'no /chosen in the ADT'
    chosen = chosen[0][0]
    lines = []
    for prop, prefix in FILES:
        want = chosen.get(prop)
        assert want, f'/chosen/{prop} missing: the payload was built without that pinned file'
        found = None
        for path in sorted(glob.glob(os.path.join(ext_dir, prefix + '-*.json')), reverse=True):
            if extent_words(path) == want:
                found = path
                break
        assert found, f'no saved {prefix} JSON matches /chosen/{prop}'
        digest = hashlib.sha256(open(found, 'rb').read()).hexdigest()
        lines.append(f'{prefix} {digest} {os.path.basename(found)}')
        print(f'{prop}: {len(want) // 24} extents = {os.path.basename(found)}')
    open(os.path.join(out, 'extents.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')

    # stock boot: kernel and AVB vbmeta
    stock = open(stock_path, 'rb').read()
    assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 4, 'not a v4 boot image'
    ksize, rsize = struct.unpack_from('<II', stock, 8)
    assert rsize == 0, 'stock boot has a ramdisk'
    kernel = stock[4096:4096 + ksize]
    assert kernel[56:60] == b'ARM\x64', 'stock kernel is not a raw arm64 Image'
    open(os.path.join(out, 'linux.Image'), 'wb').write(kernel)
    footer = stock[-64:]
    assert footer[:4] == b'AVBf', 'stock boot has no AVB footer'
    _, _, orig, vb_off, vb_size = struct.unpack('>IIQQQ', footer[4:36])
    vbmeta = stock[vb_off:vb_off + vb_size]
    assert vbmeta[:4] == b'AVB0'
    open(os.path.join(out, 'stock_vbmeta.bin'), 'wb').write(vbmeta)
    print(f'stock kernel {ksize} bytes, image_size {struct.unpack_from("<Q", kernel, 16)[0]:#x}, '
          f'vbmeta {vb_size} bytes')


if __name__ == '__main__':
    main()
