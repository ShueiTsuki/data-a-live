"""sym.py STACKS_OUT FILE=PATH...: puts symbols on the "@ ADDR" lines of stacks.py --full output (run in WSL).

Each FILE=PATH maps an image path suffix from the "image" lines (e.g. libvulkan_freedreno.dylib) to a local file
with symbols. Shared cache addresses go through ipsw dyld a2s (unslid with the process's slide).
"""
import re
import struct
import subprocess
import sys

IPSW = '/opt/work/ipswtool/ipsw'
DSC = '/opt/work/dsc/dyld_shared_cache_arm64'
CACHE = '/opt/work/dsc/a2s.cache'

lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
files = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a and not a.startswith('--'))
no_cache = '--no-cache' in sys.argv


def text_vmaddr(path):
    d = open(path, 'rb').read(65536)
    magic, cpu, sub, ftype, ncmds, size, flags, res = struct.unpack_from('<8I', d, 0)
    off = 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<2I', d, off)
        if cmd == 0x19:
            seg = d[off + 8:off + 24].rstrip(b'\0')
            if seg == b'__TEXT':
                return struct.unpack_from('<Q', d, off + 24)[0]
        off += cs
    return 0


procs = []          # (slide, [(base, path)])
cur = None
want = {}           # (proc index, addr) -> None
for ln in lines:
    m = re.match(r'==== \S+: .* slide (0x[0-9a-f]+)', ln)
    if m:
        cur = (int(m.group(1), 16), [])
        procs.append(cur)
        continue
    m = re.match(r'\s+image (0x[0-9a-f]+) (\S+)', ln)
    if m and cur:
        cur[1].append((int(m.group(1), 16), m.group(2)))

memo = {}


def where(proc, a):
    best = None
    for base, path in proc[1]:
        if base <= a and (best is None or base > best[0]):
            best = (base, path)
    return best


def symbolize_file(path, addrs):
    """Nearest preceding text symbol (the builds keep a symbol table but no debug info llvm-symbolizer reads)."""
    import bisect
    out = {}
    if not addrs:
        return out
    vm = text_vmaddr(path)
    nm = subprocess.run(['llvm-nm', '-n', '-C', path], capture_output=True, text=True).stdout
    starts, names = [], []
    for ln in nm.splitlines():
        p = ln.split(' ', 2)
        if len(p) == 3 and p[1] in 'tT' and p[0].strip():
            starts.append(int(p[0], 16))
            names.append(p[2])
    for a in addrs:
        i = bisect.bisect_right(starts, a + vm) - 1
        out[a] = '%s+%#x' % (names[i][:160], a + vm - starts[i]) if i >= 0 else '?'
    return out


def symbolize_cache(slide, a):
    u = a - slide
    r = subprocess.run([IPSW, 'dyld', 'a2s', DSC, '0x%x' % u, '--cache', CACHE, '-i'], capture_output=True, text=True)
    txt = [x for x in (r.stdout + r.stderr).splitlines() if x.strip() and 'Address location' not in x and 'Loading' not in x
           and 'symbol cache' not in x.lower()]
    lib = re.search(r'(?:dylib|image)=(\S+)', r.stdout + r.stderr)
    s = txt[-1].split(': ', 1)[-1] if txt else '?'
    return ('%s %s' % (lib.group(1).split('/')[-1] if lib else '', s)).strip()


# collect per image
per = {}
pi = -1
for ln in lines:
    if ln.startswith('==== '):
        pi += 1
    m = re.match(r'\s+@ ([0-9a-f]+)$', ln)
    if m:
        a = int(m.group(1), 16)
        if a < 0x180000000:
            w = where(procs[pi], a)
            if w:
                per.setdefault(w[1], {}).setdefault(a - w[0], None)
for path, offs in per.items():
    for suf, local in files.items():
        if path.endswith(suf):
            res = symbolize_file(local, list(offs))
            offs.update(res)

pi = -1
for ln in lines:
    if ln.startswith('==== '):
        pi += 1
    m = re.match(r'(\s+@ )([0-9a-f]+)$', ln)
    if not m:
        print(ln)
        continue
    a = int(m.group(2), 16)
    if a == 0:
        continue
    if a >= 0x180000000 and no_cache:
        print('%s%x  (shared cache %#x)' % (m.group(1), a, a - procs[pi][0]))
        continue
    if a >= 0x180000000:
        k = ('c', procs[pi][0], a)
        if k not in memo:
            memo[k] = symbolize_cache(procs[pi][0], a)
        print('%s%x  %s' % (m.group(1), a, memo[k]))
    else:
        w = where(procs[pi], a)
        if not w:
            print('%s%x  ?' % (m.group(1), a))
            continue
        s = per.get(w[1], {}).get(a - w[0])
        print('%s%x  %s+%#x %s' % (m.group(1), a, w[1].split('/')[-1], a - w[0], s or ''))
