"""Pack an arm64 Image into an Android boot image, header v4, kernel only (no ramdisk).

    python mkbootimg_v4.py Image out.img
Header layout from AOSP bootimg.h (boot_img_hdr_v4, 1584 bytes, 4096-byte pages).
os_version is 0, as in the phone's stock boot_b.
"""
import struct
import sys

PAGE = 4096


def main():
    kernel = open(sys.argv[1], 'rb').read()
    assert kernel[56:60] == b'ARM\x64', 'not an arm64 Image'
    assert struct.unpack_from('<Q', kernel, 16)[0] >= len(kernel), 'image_size smaller than the file'
    hdr = struct.pack('<8sIIII4II1536sI', b'ANDROID!', len(kernel), 0, 0, 1584,
                      0, 0, 0, 0, 4, b'', 0)
    assert len(hdr) == 1584
    pad = lambda b: b + b'\0' * (-len(b) % PAGE)
    open(sys.argv[2], 'wb').write(pad(hdr) + pad(kernel))


if __name__ == '__main__':
    main()
