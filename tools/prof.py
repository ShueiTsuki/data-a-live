#!/usr/bin/env python3
"""Name the samples of the kernel's sampling profiler (patch 0222, boot-arg pandora_prof=NAME).

  tools/prof.py LOG [--plugin NativeAdrenoMetal.symbols] [--turnip libvulkan_freedreno.dylib]
                    [--kernel kernel.development.pandora.unstripped] [--reports N] [--top N]

Run in WSL. LOG is a device log (device/logs/NAME.txt). The kernel logged, every period, the hottest PCs of the
process: U (in user code), E (the user PC a thread entered the kernel from), K (kernel PCs), and dyld's images
outside the shared cache with their load addresses, the shared cache's slide and the kernel's slide. Each PC is
named from its image: the plugin and Turnip from the local builds (their symbol tables, llvm-symbolizer), the
shared cache from the iPhone10,3 cache (ipsw dyld symaddr per image, kept under /opt/work/qcdis/syms), the kernel
from the local kernel build. Prints, per kind, the functions by samples (all reports summed, or the last N).
"""
import argparse, bisect, collections, os, re, subprocess, sys

IPSW = '/opt/work/ipswtool/ipsw'
DSC = '/opt/work/dsc/dyld_shared_cache_arm64'
DYLIBS = '/opt/work/qcdis/dylibs_v.txt'
SYMS = '/opt/work/qcdis/syms'
SYMBOLIZER = '/usr/bin/llvm-symbolizer'


def parse_log(path, last):
    images, reports, cache_slide, cache_base, kernel_slide = {}, [], 0, 0, 0
    cur = None
    for line in open(path, errors='replace'):
        m = re.search(r'PANDORA PROF IMAGES \d+, shared cache base (0x[0-9a-f]+) slide (0x[0-9a-f]+), kernel slide (0x[0-9a-f]+|0)', line)
        if m:
            cache_base, cache_slide, kernel_slide = (int(x, 0) for x in m.groups())
            continue
        m = re.search(r'PANDORA PROF IMAGE (0x[0-9a-f]+) (\S.*)$', line)
        if m:
            images[int(m.group(1), 16)] = m.group(2).strip()
            continue
        m = re.search(r'PANDORA PROF: (\d+) s of', line)
        if m:
            cur = collections.Counter()
            reports.append(cur)
            continue
        m = re.search(r'PANDORA PROF ([UEK]) (0x[0-9a-f]+) (\d+)', line)
        if m and cur is not None:
            cur[(m.group(1), int(m.group(2), 16))] += int(m.group(3))
    if last:
        reports = reports[-last:]
    total = collections.Counter()
    for r in reports:
        total.update(r)
    return images, total, cache_base, cache_slide, kernel_slide, len(reports)


def load_dylibs():
    bases = []
    for line in open(DYLIBS, errors='replace'):
        m = re.match(r'\s*\d+: (0x[0-9a-f]+)\s+\S+\s+\(\S+\)\s+(\S.*)$', line)
        if m:
            bases.append((int(m.group(1), 16), m.group(2).strip()))
    bases.sort()
    return bases


_symcache = {}


def image_symbols(path):
    """Sorted (address, name) of a shared cache image, from ipsw dyld symaddr (kept on disk)."""
    if path in _symcache:
        return _symcache[path]
    os.makedirs(SYMS, exist_ok=True)
    name = os.path.basename(path)
    f = os.path.join(SYMS, name + '.sym')
    if not os.path.exists(f):
        out = subprocess.run([IPSW, 'dyld', 'symaddr', DSC, '--image', name], capture_output=True, text=True).stdout
        open(f, 'w').write(out)
    syms = []
    for line in open(f, errors='replace'):
        m = re.match(r'^(0x[0-9a-f]+):\s+\(([^)]*)\)\s+(\S+)', line)
        if m and '__text' in m.group(2):
            syms.append((int(m.group(1), 16), m.group(3)))
    syms.sort()
    _symcache[path] = syms
    return syms


def symbolize(binary, addrs):
    """llvm-symbolizer over a local Mach-O: {addr: function}."""
    if not addrs or not binary or not os.path.exists(binary):
        return {}
    inp = '\n'.join('0x%x' % a for a in addrs) + '\n'
    out = subprocess.run([SYMBOLIZER, '--obj=' + binary, '--functions=linkage', '--demangle', '--no-inlines',
                          '--output-style=LLVM'], input=inp, capture_output=True, text=True).stdout
    names, it = {}, iter(addrs)
    for block in out.strip('\n').split('\n\n'):
        lines = block.split('\n')
        try:
            names[next(it)] = lines[0]
        except StopIteration:
            break
    return names


def short(name):
    name = re.sub(r'::h[0-9a-f]{16}$', '', name)
    name = re.sub(r'<[^<>]*>', '', name)
    name = re.sub(r'<[^<>]*>', '', name)
    return name[:150]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--plugin', default='/opt/work/lplugin/out/NativeAdrenoMetal.symbols')
    ap.add_argument('--turnip', default='/mnt/d/DeepSeekHarness/claude/ios_pandora/out-root/turnip/local215/libvulkan_freedreno.dylib')
    ap.add_argument('--kernel', default='')
    ap.add_argument('--reports', type=int, default=0)
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    images, counts, cache_base, cache_slide, kernel_slide, n = parse_log(a.log, a.reports)
    print('%d reports, %d distinct PCs, %d images outside the shared cache' % (n, len(counts), len(images)))
    local = {'NativeAdrenoMetal': a.plugin, 'libvulkan_freedreno.dylib': a.turnip}
    loads = sorted(images.items())
    dylibs = load_dylibs()
    dbases = [b for b, _ in dylibs]

    # Which image each user PC is in, and its address there.
    by_image = collections.defaultdict(set)
    where = {}
    for (kind, pc) in counts:
        if kind == 'K':
            where[(kind, pc)] = ('kernel', pc - kernel_slide)
            by_image['kernel'].add(pc - kernel_slide)
            continue
        if cache_base and cache_base <= pc < cache_base + (8 << 30):
            un = pc - cache_slide
            i = bisect.bisect_right(dbases, un) - 1
            img = dylibs[i][1] if i >= 0 else '?'
            where[(kind, pc)] = (img, un)
            by_image[img].add(un)
            continue
        i = bisect.bisect_right([b for b, _ in loads], pc) - 1
        if i < 0:
            where[(kind, pc)] = ('?', pc)
            continue
        base, path = loads[i]
        where[(kind, pc)] = (path, pc - base)
        by_image[path].add(pc - base)
    names = {}
    for img, addrs in by_image.items():
        b = os.path.basename(img)
        if img == 'kernel':
            for ad, nm in symbolize(a.kernel, sorted(addrs)).items():
                names[(img, ad)] = nm
        elif b in local:
            for ad, nm in symbolize(local[b], sorted(addrs)).items():
                names[(img, ad)] = nm
        elif img.startswith('/') and not any(img == p for p in images.values()):
            syms = image_symbols(img)
            sa = [s for s, _ in syms]
            for ad in addrs:
                j = bisect.bisect_right(sa, ad) - 1
                names[(img, ad)] = syms[j][1] if j >= 0 else '?'
    for kind, title in (('U', 'in user code'), ('E', 'in the kernel, by the user PC that entered it'),
                        ('K', 'in the kernel, by kernel PC')):
        funcs = collections.Counter()
        by_img = collections.Counter()
        total = 0
        for (k, pc), c in counts.items():
            if k != kind:
                continue
            total += c
            img, ad = where[(k, pc)]
            funcs['%s  [%s]' % (short(names.get((img, ad), hex(ad))), os.path.basename(img))] += c
            by_img[os.path.basename(img)] += c
        if not total:
            continue
        print('\n== %s: %d samples' % (title, total))
        print('  by image: ' + ', '.join('%s %.1f%%' % (i, 100.0 * c / total) for i, c in by_img.most_common(12)))
        for f, c in funcs.most_common(a.top):
            print('  %5.1f%%  %6d  %s' % (100.0 * c / total, c, f))


if __name__ == '__main__':
    main()
