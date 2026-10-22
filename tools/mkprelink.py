#!/usr/bin/env python3
"""Prelink Apple's original kexts from the iPhone X kernelcache into the PANDORA kernel.

  mkprelink.py NATIVE_KERNEL KERNELCACHE OUT [--root BUNDLE]... [--slide -0x1000000]
  mkprelink.py --verify NATIVE_KERNEL KERNELCACHE OUT

The copy and pointer rebinding are UniXNU's NativePrelink (tools/native_prelink, unchanged):
the selected kexts keep their original layout, moved by one slide below our kernel, and
every pointer from them into the kernel is bound by symbol name to our kernel's export.
Added here for PANDORA:
  * the empty __CTF segment of our build (vmsize 0, filesize > 0) is cleared first;
  * only AMFI keeps its IOKit personalities, so none of the other original drivers
    (AppleARMPlatform's CPU, NVRAM and backlight drivers, IOStorageFamily, ...) start;
    they are only libraries for AMFI here;
  * class names that our kernel already has are renamed in the original's string data
    (AppleARMPlatform's AppleARMBacklight; ours is iokit/Kernel/arm/SweetFramebuffer.cpp),
    because a second class of the same name makes the whole kext fail to load;
  * --verify checks the result against both inputs (also run after every build).
OUT gets OUT.json with the plan and the checks. Apple's code: keep OUT out of git.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'native_prelink'))
import native_prelink as npl  # noqa: E402
import emit_prelink as epl  # noqa: E402

AMFI = 'com.apple.driver.AppleMobileFileIntegrity'
# AppleImage4's IOService start opens the nonce gate launchd's ignition waits on (0155).
KEEP_PERSONALITIES = {AMFI, 'com.apple.security.AppleImage4'}
RENAMES = {'com.apple.driver.AppleARMPlatform': {'AppleARMBacklight': 'XppleARMBacklight'}}
SLIDE = -0x1000000
# iOS 16 AMFI's exported initializer (registers its MAC policy and the amfi interface), which
# the original Sandbox kext calls while the security extensions load. Without that kext the
# kernel calls it (patch 0151, UniXNU's early security initializer) when it has the ABI marker.
EARLY_SECURITY_INIT = {AMFI: 0xfffffff005704320}
M64 = (1 << 64) - 1


def clear_ctf(data):
    # By hand: native_prelink.macho() rejects exactly this segment (filesize > vmsize).
    d = bytearray(data)
    ncmds, = struct.unpack_from('<I', d, 16)
    at = 32
    for _ in range(ncmds):
        cmd, length = struct.unpack_from('<II', d, at)
        if cmd == 0x19 and d[at + 8:at + 24].rstrip(b'\0') == b'__CTF':
            vmsize, filesize = struct.unpack_from('<Q8xQ', d, at + 32)
            if vmsize == 0 and filesize:
                struct.pack_into('<Q', d, at + 48, 0)
                nsects, = struct.unpack_from('<I', d, at + 64)
                for k in range(nsects):
                    struct.pack_into('<Q', d, at + 72 + 80 * k + 40, 0)
        at += length
    return bytes(d)


def kext_headers(kc, kc_meta, entries, ident):
    va = entries[ident]['_PrelinkExecutableLoadAddr']
    return npl.macho(kc, npl.backed_offset(kc_meta['segments'], va, 32, len(kc)))


def info_segment(image):
    m = npl.macho(image)
    seg = next(s for s in m['segments'] if s['name'] == '__PRELINK_INFO')
    return seg, npl.unserialize_xml(image[seg['fileoff']:seg['fileoff'] + seg['filesize']])


def write_info(image, metadata):
    seg, _ = info_segment(image)
    xml = ET.tostring(epl.to_xml(metadata), encoding='utf-8').replace(b' />', b'/>') + b'\0'
    if len(xml) > seg['filesize']:
        raise ValueError('rewritten prelink info larger than its segment')
    image[seg['fileoff']:seg['fileoff'] + seg['filesize']] = xml + bytes(seg['filesize'] - len(xml))


# ---- minimal ARM64 decoding: class names passed to OSMetaClass::OSMetaClass ----

def _signed(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def stub_targets(blob, va, segs, image):
    """__stubs entries (ADRP x16; LDR x16,[x16,#o]; BR x16) -> pointer they load."""
    out = {}
    for k in range(0, len(blob) - 11, 12):
        a, b, c = struct.unpack_from('<3I', blob, k)
        if (a & 0x9f00001f) != 0x90000010 or (b & 0xffc003ff) != 0xf9400210 or c != 0xd61f0200:
            continue
        pc = va + k
        imm = _signed(((a >> 29) & 3) | ((a >> 5) & 0x7ffff) << 2, 21)
        slot = ((pc & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8) & M64
        at = npl.backed_offset(segs, slot, 8, len(image))
        out[pc] = struct.unpack_from('<Q', image, at)[0]
    return out


def class_names(image, meta, module, ctor_address):
    """Strings in x1 at each BL to a stub that loads ctor_address (OSMetaClass constructor)."""
    segs = meta['segments']
    tx = next(s for s in module['segments'] if s['name'] == '__TEXT_EXEC')
    at = npl.backed_offset(segs, tx['va'], tx['filesize'], len(image))
    blob = image[at:at + tx['filesize']]
    stubs = {pc for pc, t in stub_targets(blob, tx['va'], segs, image).items() if t == ctor_address}
    names, pages, x1 = set(), {}, None
    for k in range(0, len(blob) - 3, 4):
        w, = struct.unpack_from('<I', blob, k)
        pc = tx['va'] + k
        if (w & 0x9f000000) == 0x90000000:
            imm = _signed(((w >> 29) & 3) | ((w >> 5) & 0x7ffff) << 2, 21)
            pages[w & 31] = ((pc & ~4095) + (imm << 12)) & M64
        elif (w & 0xff800000) == 0x91000000:
            rd, rn, imm = w & 31, (w >> 5) & 31, (w >> 10) & 0xfff
            if (w >> 22) & 1:
                imm <<= 12
            if rd == 1 and rn in pages:
                x1 = (pages[rn] + imm) & M64
        elif (w & 0xfc000000) == 0x94000000:
            target = (pc + (_signed(w & 0x3ffffff, 26) << 2)) & M64
            if target in stubs and x1 is not None:
                s = npl.backed_offset(segs, x1, 1, len(image))
                names.add(image[s:image.index(b'\0', s)].decode('latin1'))
            x1 = None
            pages.clear()
    return names


def native_class_names(native, native_meta):
    out = set()
    for names in npl.exports(native, native_meta).values():
        for n in names:
            m = re.fullmatch(r'__ZN(\d+)(\w+)10gMetaClassE', n)
            if m and len(m.group(2)) == int(m.group(1)):
                out.add(m.group(2))
    return out


def build(native, kc, roots, slide):
    native = clear_ctf(native)
    names = {n for ns in npl.exports(native, npl.macho(native)).values() for n in ns}
    early = EARLY_SECURITY_INIT if '_native_prelink_security_init_version' in names else {}
    image, report = epl.emit(native, kc, roots, slide, early)
    report['early_security_init_used'] = {k: hex(v) for k, v in early.items()}
    image = bytearray(image)
    kc_meta = npl.macho(kc)
    _, kc_info = info_segment(kc)
    entries = {e['CFBundleIdentifier']: e for e in kc_info['_PrelinkInfoDictionary']}
    out_segs = npl.macho(image)['segments']
    renamed = []
    for ident, renames in RENAMES.items():
        if ident not in report['components']:
            continue
        module = kext_headers(kc, kc_meta, entries, ident)
        cstrings = [x for s in module['segments'] for x in s['sections'] if x['name'] == '__cstring']
        for old, new in renames.items():
            assert len(old) == len(new)
            needle = b'\0' + old.encode() + b'\0'
            hits = []
            for sec in cstrings:
                at = npl.backed_offset(kc_meta['segments'], sec['va'], sec['size'], len(kc))
                blob = kc[at - 1:at + sec['size']]
                i = blob.find(needle)
                while i >= 0:
                    hits.append(sec['va'] + i)
                    i = blob.find(needle, i + 1)
            if not hits:
                raise ValueError(f'{ident}: no string {old}')
            for va in hits:
                o = npl.backed_offset(out_segs, va + slide, len(old), len(image))
                assert image[o:o + len(old)] == old.encode()
                image[o:o + len(old)] = new.encode()
                renamed.append({'module': ident, 'va': va + slide, 'from': old, 'to': new})
    seg, metadata = info_segment(image)
    stripped = []
    for e in metadata['_PrelinkInfoDictionary']:
        if e['CFBundleIdentifier'] not in KEEP_PERSONALITIES and e.pop('IOKitPersonalities', None):
            stripped.append(e['CFBundleIdentifier'])
    write_info(image, metadata)
    report.update(ctf_cleared=True, renamed_strings=renamed, personalities_removed=sorted(stripped),
                  personalities_kept=sorted(KEEP_PERSONALITIES), candidate_sha256=hashlib.sha256(image).hexdigest())
    return bytes(image), native, report


def verify(native, kc, image, slide):
    """Independent checks of OUT against both inputs; returns (ok, lines)."""
    native = clear_ctf(native)
    lines, ok = [], True

    def check(cond, text):
        nonlocal ok
        ok &= bool(cond)
        lines.append(('PASS ' if cond else 'FAIL ') + text)

    nm, km, om = npl.macho(native), npl.macho(kc), npl.macho(image)
    _, kc_info = info_segment(kc)
    entries = {e['CFBundleIdentifier']: e for e in kc_info['_PrelinkInfoDictionary']}
    _, info = info_segment(image)
    new_entries = {e['CFBundleIdentifier']: e for e in info['_PrelinkInfoDictionary']}
    comps = sorted(i for i, e in new_entries.items() if e.get('_PrelinkExecutableLoadAddr') and e.get('OSKernelResource') is not True)
    lines.append('components: ' + ', '.join(comps))

    # 1. Our own kernel's segments are untouched (only the load commands grew).
    hdr_end = 32 + struct.unpack_from('<I', image, 20)[0]
    first = min(x['fileoff'] for s in nm['segments'] for x in s['sections'] if x['size'] and x['fileoff'])
    same = all(image[max(s['fileoff'], first):s['fileoff'] + s['filesize']] ==
               native[max(s['fileoff'], first):s['fileoff'] + s['filesize']]
               for s in nm['segments'] if s['filesize'])
    check(same, 'native kernel segment bytes unchanged')
    check(hdr_end <= first and image[hdr_end:first] == native[hdr_end:first],
          'header padding after the load commands unchanged')

    # 2. Pointer rebinding: every relocation site of the selected kexts.
    offsets_blob = info['_PrelinkLinkKASLROffsets']
    count, = struct.unpack_from('<I', offsets_blob)
    offsets = struct.unpack_from(f'<{count}I', offsets_blob, 4)
    plt = next(s for s in om['segments'] if s['name'] == '__PRELINK_TEXT')
    sites = {plt['va'] + o for o in offsets}
    extents = []
    allowed = {}  # output va -> why the byte may differ from the original
    native_exports = npl.exports(native, nm)
    kc_exports = npl.exports(kc, km)
    for ident in comps:
        mod = npl.macho(kc, npl.backed_offset(km['segments'], entries[ident]['_PrelinkExecutableLoadAddr'], 32, len(kc)))
        for s in mod['segments']:
            if s['size'] and s['name'] != '__LINKEDIT':
                extents.append((s['va'] + slide, s['va'] + slide + s['size'], ident, s))
    def owner(va):
        return next((e for e in extents if e[0] <= va < e[1]), None)
    kernel_ptrs = comp_ptrs = bad = 0
    for site in sites:
        o = npl.backed_offset(om['segments'], site, 8, len(image))
        new, = struct.unpack_from('<Q', image, o)
        old_o = npl.backed_offset(km['segments'], site - slide, 8, len(kc))
        old, = struct.unpack_from('<Q', kc, old_o)
        for b in range(8):
            allowed[site + b] = 'pointer'
        if owner(site) is None:
            bad += 1
        elif owner(old + slide) is not None:   # extents are in the new (slid) addresses
            comp_ptrs += 1
            bad += new != old + slide or owner(new) is None
        else:
            kernel_ptrs += 1
            bad += not (set(kc_exports.get(old, [])) & set(native_exports.get(new, [])))
    check(bad == 0, f'{len(sites)} relocation sites: {comp_ptrs} inside the kexts (slid), '
          f'{kernel_ptrs} into our kernel (same exported symbol); {bad} wrong')

    # 3. Original bytes: code identical; data differs only at pointers, header VAs,
    #    kmod_info.address and the renamed class strings.
    renamed = 0
    for ident, rn in RENAMES.items():
        if ident not in comps:
            continue
        mod = npl.macho(kc, npl.backed_offset(km['segments'], entries[ident]['_PrelinkExecutableLoadAddr'], 32, len(kc)))
        for old, new in rn.items():
            needle = b'\0' + old.encode() + b'\0'
            for sec in (x for s in mod['segments'] for x in s['sections'] if x['name'] == '__cstring'):
                at = npl.backed_offset(km['segments'], sec['va'], sec['size'], len(kc))
                blob = kc[at - 1:at + sec['size']]
                i = blob.find(needle)
                while i >= 0:
                    va = sec['va'] + i + slide
                    o = npl.backed_offset(om['segments'], va, len(new), len(image))
                    renamed += image[o:o + len(new)] == new.encode()
                    for b in range(len(new)):
                        if old[b] != new[b]:
                            allowed[va + b] = 'rename'
                    i = blob.find(needle, i + 1)
    text_ok, other = True, []
    for ident in comps:
        e = entries[ident]
        hdr = npl.backed_offset(km['segments'], e['_PrelinkExecutableLoadAddr'], 32, len(kc))
        mod = npl.macho(kc, hdr)
        hdr_va = e['_PrelinkExecutableLoadAddr'] + slide
        for s in mod['segments']:
            for b in range(8):
                allowed[hdr_va + s['command_offset'] - hdr + 24 + b] = 'header'
            for sec in s['sections']:
                for b in range(8):
                    allowed[hdr_va + sec['command_offset'] - hdr + 32 + b] = 'header'
        if e.get('_PrelinkKmodInfo'):
            for b in range(8):
                allowed[e['_PrelinkKmodInfo'] + slide + 156 + b] = 'kmod'
        for s in mod['segments']:
            if not s['filesize'] or s['name'] == '__LINKEDIT':
                continue
            a = npl.backed_offset(km['segments'], s['va'], s['filesize'], len(kc))
            b = npl.backed_offset(om['segments'], s['va'] + slide, s['filesize'], len(image))
            x, y = kc[a:a + s['filesize']], image[b:b + s['filesize']]
            if s['name'] == '__TEXT_EXEC':
                text_ok &= x == y
                continue
            if x == y:
                continue
            for i in range(len(x)):
                if x[i] != y[i] and (s['va'] + slide + i) not in allowed:
                    other.append((ident, s['name'], s['va'] + slide + i))
    check(text_ok, 'original executable segments (__TEXT_EXEC) byte-identical')
    check(not other, f'data differs only at pointers, header VAs, kmod address and renamed strings '
          f'({len(other)} other bytes: {[(i, s, hex(v)) for i, s, v in other[:4]]})')
    check(renamed >= 1, f'{renamed} class-name strings renamed '
          f'({", ".join(f"{old}->{new}" for rn in RENAMES.values() for old, new in rn.items())})')

    # 4. Sparse header index (__PRELINK_TEXT,__macho_list) lists exactly the component headers.
    sec = next(x for x in plt['sections'] if x['name'] == '__macho_list')
    heads = struct.unpack_from(f'<{sec["size"] // 8}Q', image, sec['fileoff'])
    want = sorted(new_entries[i]['_PrelinkExecutableLoadAddr'] for i in comps)
    good = list(heads) == want and all(
        npl.macho(image, npl.backed_offset(om['segments'], h, 32, len(image)))['kind'] == 11 for h in heads)
    check(good, f'__macho_list: {len(heads)} kext headers, all MH_KEXT_BUNDLE at their new addresses')

    # 5. Class names: no class defined twice (ours vs. the original kexts', and between kexts).
    ctor = [a for a, n in native_exports.items() if '__ZN11OSMetaClassC2EPKcPKS_j' in n]
    ours = native_class_names(native, nm)
    seen, clashes = {}, []
    for ident in comps:
        mod = npl.macho(image, npl.backed_offset(om['segments'], new_entries[ident]['_PrelinkExecutableLoadAddr'], 32, len(image)))
        for n in class_names(image, om, mod, ctor[0] if ctor else None):
            if n in ours or n in seen:
                clashes.append((ident, n))
            seen[n] = ident
    check(len(ctor) == 1 and not clashes, f'{len(seen)} original classes, none clash with our {len(ours)} classes: {clashes}')

    # 6. C++ ABI: the kexts call methods of our kernel's IOKit/libkern objects by vtable slot,
    #    so every kernel class vtable must list the same methods in the same slots as the
    #    original kernel's (an XNU_TARGET_OS_OSX build adds 16 reserved slots per class).
    oby = {n: a for a, ns in kc_exports.items() for n in ns}
    nby = {n: a for a, ns in native_exports.items() for n in ns}
    k_text = [s for s in km['segments'] if s['name'] == '__TEXT_EXEC']

    def kc_code(v):
        return any(s['va'] <= v < s['va'] + s['size'] for s in k_text)

    def rd(img, meta, va):
        return struct.unpack_from('<Q', img, npl.backed_offset(meta['segments'], va, 8, len(img)))[0]

    compared, bad_classes = 0, []
    for name, ova in sorted(oby.items()):
        if not name.startswith('__ZTV') or name not in nby:
            continue
        compared += 1
        for slot in range(512):
            ot = rd(kc, km, ova + 16 + slot * 8)
            if not kc_code(ot):
                break
            onames = set(kc_exports.get(ot, []))
            nnames = set(native_exports.get(rd(image, om, nby[name] + 16 + slot * 8), []))
            if onames and not onames & nnames:
                bad_classes.append((name, slot, sorted(onames)[0], sorted(nnames)[:1]))
                break
    check(not bad_classes, f'{compared} kernel class vtables slot-compatible with the original '
          f'({len(bad_classes)} differ, first: {bad_classes[:3]})')

    # Early security initializer: the original address plus the slide, inside AMFI's code.
    early = new_entries[AMFI].get('_NativePrelinkEarlySecurityInit') if AMFI in new_entries else None
    if early is not None:
        check(early == EARLY_SECURITY_INIT[AMFI] + slide,
              f'AMFI early security initializer {early:#x} (original {EARLY_SECURITY_INIT[AMFI]:#x})')
    else:
        lines.append('      no early security initializer (kernel without patch 0151)')

    # 7. Personalities: only the kept ones can start a driver.
    with_pers = sorted(i for i, e in new_entries.items() if e.get('IOKitPersonalities'))
    check(set(with_pers) <= KEEP_PERSONALITIES, f'IOKit personalities only in {with_pers}')

    # 8. Layout: the kexts sit below our kernel and above nothing of ours.
    low = min(s['va'] for s in om['segments'] if s['size'])
    lines.append(f'      lowest VA {low:#x}; our __TEXT {nm["segments"][0]["va"]:#x}; '
                 f'image {len(image)} bytes')
    for s in om['segments']:
        if s['name'].startswith(('__PRELINK', '__PLK')) and s['size']:
            lines.append(f'      {s["name"]:16} {s["va"]:#x} size {s["size"]:#x}')
    return ok, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('native'); ap.add_argument('kernelcache'); ap.add_argument('out')
    ap.add_argument('--root', action='append', default=[])
    ap.add_argument('--slide', type=lambda v: int(v, 0), default=SLIDE)
    ap.add_argument('--verify', action='store_true', help='only check an existing OUT')
    a = ap.parse_args()
    native, kc = open(a.native, 'rb').read(), open(a.kernelcache, 'rb').read()
    roots = a.root or [AMFI]
    if not a.verify:
        image, _, report = build(native, kc, roots, a.slide)
        open(a.out, 'wb').write(image)
    image = open(a.out, 'rb').read()
    ok, lines = verify(native, kc, image, a.slide)
    print('\n'.join(lines))
    if not a.verify:
        report['verify'] = lines
        report.pop('bindings', None)
        with open(a.out + '.json', 'w') as f:
            json.dump(report, f, indent=1, default=str)
    print('mkprelink:', 'OK' if ok else 'CHECKS FAILED', a.out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
