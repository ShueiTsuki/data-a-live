"""The stock parts of the boot menu image.

    python -I prep.py STOCK_BOOT OUTDIR

STOCK_BOOT  the phone's stock boot_b (a full 96 MiB partition copy)

Writes to OUTDIR:
  linux.Image       the stock Android kernel, byte for byte
  stock_vbmeta.bin  the stock AVB vbmeta blob (boot os_version / security_patch props)
"""
import os
import struct
import sys


def main():
    stock_path, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    stock = open(stock_path, 'rb').read()
    assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 4, 'not a v4 boot image'
    ksize, rsize = struct.unpack_from('<II', stock, 8)
    assert rsize == 0, 'stock boot has a ramdisk'
    kernel = stock[4096:4096 + ksize]
    assert kernel[56:60] == b'ARM\x64', 'stock kernel is not a raw arm64 Image'
    open(os.path.join(out, 'linux.Image'), 'wb').write(kernel)
    footer = stock[-64:]
    assert footer[:4] == b'AVBf', 'stock boot has no AVB footer'
    _, _, _, vb_off, vb_size = struct.unpack('>IIQQQ', footer[4:36])
    vbmeta = stock[vb_off:vb_off + vb_size]
    assert vbmeta[:4] == b'AVB0'
    open(os.path.join(out, 'stock_vbmeta.bin'), 'wb').write(vbmeta)
    print(f'stock kernel {ksize} bytes, image_size {struct.unpack_from("<Q", kernel, 16)[0]:#x}, '
          f'vbmeta {vb_size} bytes')


if __name__ == '__main__':
    main()
