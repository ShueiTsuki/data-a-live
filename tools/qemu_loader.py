#!/usr/bin/env python3
"""Boot the real phone image (shim/loader out/Image) in QEMU virt, phone addresses included.

  qemu_loader.py IMAGE PAYLOAD OUTDIR [--secs N] [--kernel KERNEL] [--hmp CMD]...

QEMU starts the arm64 Image like ABL does (EL1, MMU off, x0 = dtb). virt RAM starts at
0x40000000; it is sized to reach 0xc00000000, so the phone framebuffer (0xfc800000) and the
kernel RAM (0x8c0000000 + 5 GiB) are plain memory. The phone's GIC/UART/etc. are not there:
this checks the loader and the kernel's early boot, not drivers.

Writes fb.png (1220x2656, what the phone would show), fb.txt (decoded fbcon text when
--kernel gives the unstripped kernel with _iso_font), regs.txt, qemu.log.
"""
import argparse, json, os, socket, struct, subprocess, sys, time, zlib

FB_BASE, FB_W, FB_H = 0xfc800000, 1220, 2656
FB_STRIDE = FB_W * 4
RAM_TOP = 0xc00000000


def qmp(sock_path, *cmds, pmemsave=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    f = s.makefile('rw')
    f.readline()

    def call(execute, **args):
        f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                return r

    call('qmp_capabilities')
    out = []
    for c in cmds:
        r = call('human-monitor-command', **{'command-line': c})
        out.append(r.get('return', repr(r.get('error'))))
    if pmemsave:
        r = call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
        if 'error' in r:
            print(f'qmp pmemsave: {r["error"]}', file=sys.stderr)
    s.close()
    return out


def write_png(path, raw):
    rows = []
    for y in range(FB_H):
        line = raw[y * FB_STRIDE:(y + 1) * FB_STRIDE]
        rgb = bytearray(FB_W * 3)	# a8r8g8b8 little endian = B G R A in memory
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b'\0' + bytes(rgb))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', FB_W, FB_H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def payload_data(payload, va, size):
    nsegs = struct.unpack_from('<I', payload, 12)[0]
    for i in range(nsegs):
        vmaddr, _, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        if vmaddr <= va and va + size <= vmaddr + fsize:
            return payload[foff + va - vmaddr:foff + va - vmaddr + size]
    raise ValueError(f'VA {va:#x} not in payload')


def decode_screen(raw, font):
    """pe_serial.c's fbcon: 2x scaled 8x16 glyphs, white on black."""
    glyphs = {font[c * 16:(c + 1) * 16]: chr(c) for c in range(32, 127)}
    rows = []
    for row in range(FB_H // 32):
        chars = []
        for col in range(FB_W // 16):
            glyph = bytearray(16)
            for y in range(16):
                o = (row * 32 + y * 2) * FB_STRIDE + col * 16 * 4
                for x in range(8):
                    if raw[o + x * 8:o + x * 8 + 4] == b'\xff' * 4:
                        glyph[y] |= 1 << x
            chars.append(glyphs.get(bytes(glyph), '?'))
        rows.append(''.join(chars).rstrip())
    return '\n'.join(rows).rstrip() + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('payload')
    ap.add_argument('outdir')
    ap.add_argument('--secs', type=float, default=40)
    ap.add_argument('--kernel')
    ap.add_argument('--cpu', default='cortex-a76')
    ap.add_argument('--hmp', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.abspath(a.outdir)
    payload = open(a.payload, 'rb').read()
    font = None
    if a.kernel:
        syms = subprocess.check_output(['llvm-nm', '--defined-only', a.kernel], text=True)
        fonts = [int(l.split()[0], 16) for l in syms.splitlines() if l.split()[-1:] == ['_iso_font']]
        font = payload_data(payload, fonts[0], 256 * 16) if len(fonts) == 1 else None

    mon = f'{out}/mon.sock'
    if os.path.exists(mon):
        os.unlink(mon)
    ram_gib = -(-(RAM_TOP - 0x40000000) // (1 << 30))
    cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', a.cpu, '-smp', '1',
           '-m', f'{ram_gib}G', '-nographic', '-serial', f'file:{out}/serial.txt',
           '-qmp', f'unix:{mon},server,nowait', '-d', 'int,guest_errors,unimp', '-D', f'{out}/qemu.log',
           '-no-reboot', '-kernel', a.image]
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < a.secs:
        time.sleep(0.2)
    if p.poll() is not None:
        print(f'qemu exited: {p.returncode} (PSCI reset = loader fatal or kernel reboot)')
        return 1
    _, regs, *extra = qmp(mon, 'stop', 'info registers', *a.hmp,
                          pmemsave=(FB_BASE, FB_STRIDE * FB_H, f'{out}/fb.raw'))
    open(f'{out}/regs.txt', 'w').write(regs + ''.join(f'\n(qemu) {c}\n{r}' for c, r in zip(a.hmp, extra)))
    p.kill()
    p.wait()
    raw = open(f'{out}/fb.raw', 'rb').read()
    write_png(f'{out}/fb.png', raw)
    if font:
        open(f'{out}/fb.txt', 'w').write(decode_screen(raw, font))
    os.unlink(f'{out}/fb.raw')
    return 0


if __name__ == '__main__':
    sys.exit(main())
