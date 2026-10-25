#!/usr/bin/env python3
"""Name the samples of the kernel's sampling profiler (patch 0222, call stacks 0223; boot-arg pandora_prof=NAME).

  tools/prof.py LOG [--plugin NativeAdrenoMetal.symbols] [--turnip libvulkan_freedreno.symbols]
                    [--kernel kernel.development.pandora.unstripped] [--reports N] [--top N]
                    [--callers REGEX] [--image NAME]

Run in WSL. LOG is a device log (device/logs/NAME.txt). The kernel logged, every period, the PCs of the process:
U (in user code), E (the user PC a thread entered the kernel from), K (kernel PCs); with 0223 instead the kernel
chains (KC: kernel PC, LR, frame records' returns) and the user call stacks (S: samples in user code, samples in the
kernel before it returned, the leaf's LR when no frame record holds it, PC, returns), U/E/K made from their first
PCs; and dyld's images outside the shared
cache with their load addresses, the shared cache's slide and the kernel's slide. Each address is named from its
image: the plugin and Turnip from the local builds (their symbol tables, llvm-symbolizer), the shared cache from the
iPhone10,3 cache (ipsw dyld symaddr per image, kept under /opt/work/qcdis/syms), the kernel from the local kernel
build. Prints, per kind, the functions by samples (all reports summed, or the last N); from the call stacks, each
function's inclusive samples (it or anything it called), the callers of the functions --callers names, and the time
in the kernel by the user functions that entered it; from the kernel chains, the callers of the hottest kernel
functions. --image keeps one image's functions in the self-time lists.
"""
import argparse, bisect, collections, os, re, subprocess, sys

IPSW = '/opt/work/ipswtool/ipsw'
DSC = '/opt/work/dsc/dyld_shared_cache_arm64'
DYLIBS = '/opt/work/qcdis/dylibs_v.txt'
SYMS = '/opt/work/qcdis/syms'
SYMBOLIZER = '/usr/bin/llvm-symbolizer'
CALLERS = r'^(_?malloc|_?free|_?calloc|_?realloc|_?szone_|_?nanov2_|_?_platform_mem|_?mach_msg|_?mach_absolute_time|_?objc_msgSend$|_?os_unfair_lock)'


def parse_log(path, last):
    images, reports, cache_slide, cache_base, kernel_slide = {}, [], 0, 0, 0
    cur = None
    for line in open(path, errors='replace'):
        if 'PANDORA PROF' not in line:
            continue
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
            cur = {'pc': collections.Counter(), 'kc': collections.Counter(), 's': {}}
            reports.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r'PANDORA PROF ([UEK]) (0x[0-9a-f]+) (\d+)', line)
        if m:
            cur['pc'][(m.group(1), int(m.group(2), 16))] += int(m.group(3))
            continue
        m = re.search(r'PANDORA PROF KC (\d+)((?: [0-9a-f]+)+)\s*$', line)
        if m:
            cur['kc'][tuple(int(x, 16) for x in m.group(2).split())] += int(m.group(1))
            continue
        m = re.search(r'PANDORA PROF S (\d+) (\d+) ([0-9a-f]+)((?: [0-9a-f]+)+)\s*$', line)
        if m:
            key = (int(m.group(3), 16), tuple(int(x, 16) for x in m.group(4).split()))
            u, k = cur['s'].get(key, (0, 0))
            cur['s'][key] = (u + int(m.group(1)), k + int(m.group(2)))
    if last:
        reports = reports[-last:]
    total = {'pc': collections.Counter(), 'kc': collections.Counter(), 's': {}}
    for r in reports:
        total['pc'].update(r['pc'])
        total['kc'].update(r['kc'])
        for key, (u, k) in r['s'].items():
            pu, pk = total['s'].get(key, (0, 0))
            total['s'][key] = (pu + u, pk + k)
        if not r['pc']:
            # 0223 logs no U/E/K lines: they are the stacks' first PCs (in user code; the user PC a kernel sample
            # entered from, where the thread returns) and the chains' first PCs (kernel).
            for (lr, f), (u, k) in r['s'].items():
                if u:
                    total['pc'][('U', f[0])] += u
                if k:
                    total['pc'][('E', f[0])] += k
            for f, c in r['kc'].items():
                total['pc'][('K', f[0])] += c
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
    """A function's name without Rust's hash and generic arguments: <T as Trait>::f as T::f, a drop by its type."""
    name = re.sub(r'::h[0-9a-f]{16}$', '', re.sub(r' \(\.llvm\.\d+\)$', '', name))
    m = re.match(r'^core::ptr::drop_(?:in_place|glue)(?:::)?<(.*)>$', name)
    if m:
        return 'drop ' + short(m.group(1))
    for _ in range(6):
        name = re.sub(r'<([^<>]*?) as [^<>]*>', r'\1', name)
        name = re.sub(r'::<[^<>]*>|(?<=[\w>])<[^<>]*>', '', name)
    name = re.sub(r'^<([^<>]*)>', r'\1', name)
    return name[:150]


class Names:
    """Where each address is (image, address in it) and its function's name, resolved in one batch per image."""

    def __init__(self, a, images, cache_base, cache_slide, kernel_slide):
        self.a, self.cache_base, self.cache_slide, self.kernel_slide = a, cache_base, cache_slide, kernel_slide
        self.images = images
        self.loads = sorted(images.items())
        self.load_bases = [b for b, _ in self.loads]
        self.dylibs = load_dylibs()
        self.dbases = [b for b, _ in self.dylibs]
        self.local = {'NativeAdrenoMetal': a.plugin, 'libvulkan_freedreno.dylib': a.turnip}
        self.where, self.names, self.pending = {}, {}, collections.defaultdict(set)

    def place(self, addr, kernel):
        key = (kernel, addr)
        if key in self.where:
            return self.where[key]
        if kernel:
            w = ('kernel', addr - self.kernel_slide)
        elif self.cache_base and self.cache_base <= addr < self.cache_base + (8 << 30):
            un = addr - self.cache_slide
            i = bisect.bisect_right(self.dbases, un) - 1
            w = (self.dylibs[i][1] if i >= 0 else '?', un)
        else:
            i = bisect.bisect_right(self.load_bases, addr) - 1
            w = (self.loads[i][1], addr - self.loads[i][0]) if i >= 0 else ('?', addr)
        self.where[key] = w
        if (w[0], w[1]) not in self.names:
            self.pending[w[0]].add(w[1])
        return w

    def resolve(self):
        for img, addrs in self.pending.items():
            b = os.path.basename(img)
            if img == 'kernel':
                got = symbolize(self.a.kernel, sorted(addrs))
            elif b in self.local:
                got = symbolize(self.local[b], sorted(addrs))
            elif img.startswith('/') and img not in self.images.values():
                syms = image_symbols(img)
                sa = [s for s, _ in syms]
                got = {}
                for ad in addrs:
                    j = bisect.bisect_right(sa, ad) - 1
                    got[ad] = syms[j][1] if j >= 0 else '?'
            else:
                got = {}
            for ad in addrs:
                self.names[(img, ad)] = got.get(ad, '0x%x' % ad)
        self.pending.clear()

    def func(self, addr, kernel, ret=False):
        """'name  [image]' of the function holding addr; a return address is named by its call (addr - 4)."""
        img, ad = self.place(addr - 4 if ret else addr, kernel)
        if (img, ad) not in self.names:
            self.resolve()
        return '%s  [%s]' % (short(self.names.get((img, ad), hex(ad))), os.path.basename(img))


def frames(n, kernel, leaf, lr, rets):
    """A stack's functions, leaf first: the leaf's LR kept when it is another function than the leaf's and not the
    next frame record's return (a leaf without its own frame record)."""
    out = [n.func(leaf, kernel)]
    if lr and (not rets or lr != rets[0]):
        f = n.func(lr, kernel, ret=True)
        if f != out[0]:
            out.append(f)
    for r in rets:
        if r:
            out.append(n.func(r, kernel, ret=True))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--plugin', default='/opt/work/lplugin/out/NativeAdrenoMetal.symbols')
    ap.add_argument('--turnip', default='/opt/work/lturnip/out/libvulkan_freedreno.symbols')
    ap.add_argument('--kernel', default='')
    ap.add_argument('--reports', type=int, default=0)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--callers', default=CALLERS)
    ap.add_argument('--image', default='')
    ap.add_argument('--under', action='append', default=[],
                    help='a function (regex): what it calls directly, and the functions its samples end in')
    a = ap.parse_args()
    images, total, cache_base, cache_slide, kernel_slide, nrep = parse_log(a.log, a.reports)
    counts = total['pc']
    print('%d reports, %d distinct PCs, %d kernel chains, %d call stacks, %d images outside the shared cache' %
          (nrep, len(counts), len(total['kc']), len(total['s']), len(images)))
    n = Names(a, images, cache_base, cache_slide, kernel_slide)
    for (kind, pc) in counts:
        n.place(pc, kind == 'K')
    for f in total['kc']:
        n.place(f[0], True)
        for r in f[1:]:
            if r:
                n.place(r - 4, True)
    for (lr, f) in total['s']:
        n.place(f[0], False)
        for r in (lr,) + f[1:]:
            if r:
                n.place(r - 4, False)
    n.resolve()

    for kind, title in (('U', 'in user code'), ('E', 'in the kernel, by the user PC that entered it'),
                        ('K', 'in the kernel, by kernel PC')):
        funcs = collections.Counter()
        by_img = collections.Counter()
        total_k = 0
        for (k, pc), c in counts.items():
            if k != kind:
                continue
            total_k += c
            img, _ = n.place(pc, k == 'K')
            by_img[os.path.basename(img)] += c
            if a.image and os.path.basename(img) != a.image:
                continue
            funcs[n.func(pc, k == 'K')] += c
        if not total_k:
            continue
        print('\n== %s: %d samples' % (title, total_k))
        print('  by image: ' + ', '.join('%s %.1f%%' % (i, 100.0 * c / total_k) for i, c in by_img.most_common(12)))
        for f, c in funcs.most_common(a.top):
            print('  %5.1f%%  %6d  %s' % (100.0 * c / total_k, c, f))

    stacks = total['s']
    if stacks:
        su = sum(u for u, _ in stacks.values())
        sk = sum(k for _, k in stacks.values())
        incl, incl_k, self_ = collections.Counter(), collections.Counter(), collections.Counter()
        callers = collections.defaultdict(collections.Counter)
        app_callers = collections.defaultdict(collections.Counter)
        want = re.compile(a.callers)
        own = ('NativeAdrenoMetal', 'QuartzCore', 'libvulkan_freedreno.dylib')
        for (lr, f), (u, k) in stacks.items():
            fr = frames(n, False, f[0], lr, f[1:])
            w = u + k
            self_[fr[0]] += w
            for name in set(fr):
                incl[name] += w
                if k:
                    incl_k[name] += k
            # Callers of a named function: the frame below its last appearance (once per stack), and the nearest
            # frame below it in the plugin, QuartzCore or Turnip.
            seen = set()
            for i, name in enumerate(fr):
                fn = name.split('  [')[0]
                if name in seen or not want.search(fn):
                    continue
                seen.add(name)
                j = i + 1
                while j < len(fr) and fr[j] == name:
                    j += 1
                callers[name][fr[j] if j < len(fr) else '(stack ends)'] += w
                for g in fr[j:]:
                    if any(g.endswith('[%s]' % o) for o in own):
                        app_callers[name][g] += w
                        break
        print('\n== call stacks: %d samples (%d in user code, %d in the kernel)' % (su + sk, su, sk))
        print('-- inclusive (the function or anything it called), with its own samples')
        for name, c in incl.most_common(a.top * 2):
            print('  %5.1f%%  self %5.1f%%  %s' % (100.0 * c / (su + sk), 100.0 * self_[name] / (su + sk), name))
        if sk:
            print('-- time in the kernel, by the user functions on the stack that entered it (% of kernel samples)')
            for name, c in incl_k.most_common(a.top):
                print('  %5.1f%%  %s' % (100.0 * c / sk, name))
        heavy = sorted(callers, key=lambda x: -sum(callers[x].values()))
        for name in heavy[:16]:
            tot = sum(callers[name].values())
            print('-- callers of %s: %.1f%%' % (name, 100.0 * tot / (su + sk)))
            for g, c in callers[name].most_common(8):
                print('     %5.1f%%  %s' % (100.0 * c / tot, g))
            if app_callers[name]:
                print('    nearest plugin/QuartzCore/Turnip frame:')
                for g, c in app_callers[name].most_common(8):
                    print('     %5.1f%%  %s' % (100.0 * c / tot, g))

    for pattern in a.under:
        # The samples whose stack holds a function matching PATTERN (its outermost appearance): the frame it called
        # (one nearer the leaf), and the leaf function itself.
        want = re.compile(pattern)
        callees, leaves = collections.Counter(), collections.Counter()
        tot, all_w = 0, 0
        root = None
        for (lr, f), (u, k) in stacks.items():
            fr = frames(n, False, f[0], lr, f[1:])
            w = u + k
            all_w += w
            hits = [i for i, name in enumerate(fr) if want.search(name.split('  [')[0])]
            if not hits:
                continue
            i = hits[-1]
            root = root or fr[i]
            tot += w
            j = i - 1
            while j >= 0 and fr[j] == fr[i]:
                j -= 1
            callees[fr[j] if j >= 0 else '(itself)'] += w
            leaves[fr[0] if fr[0] != fr[i] else '(itself)'] += w
        if not tot:
            print('\n== under %s: no samples' % pattern)
            continue
        print('\n== under %s: %.1f%% of all samples (%s)' % (pattern, 100.0 * tot / all_w, root))
        print('-- what it calls (share of its samples)')
        for g, c in callees.most_common(a.top):
            print('  %5.1f%%  %s' % (100.0 * c / tot, g))
        print('-- where its samples end')
        for g, c in leaves.most_common(a.top):
            print('  %5.1f%%  %s' % (100.0 * c / tot, g))

    chains = total['kc']
    if chains:
        kt = sum(chains.values())
        by_leaf = collections.defaultdict(collections.Counter)
        for f, c in chains.items():
            fr = frames(n, True, f[0], f[1], f[2:])
            by_leaf[fr[0]][' <- '.join(x.split('  [')[0] for x in fr[1:5])] += c
        print('\n== kernel chains: %d samples, the hottest kernel functions and who called them' % kt)
        for leaf in sorted(by_leaf, key=lambda x: -sum(by_leaf[x].values()))[:a.top // 2]:
            tot = sum(by_leaf[leaf].values())
            print('-- %5.1f%%  %s' % (100.0 * tot / kt, leaf))
            for chain, c in by_leaf[leaf].most_common(6):
                print('     %5.1f%%  <- %s' % (100.0 * c / tot, chain))


if __name__ == '__main__':
    main()
