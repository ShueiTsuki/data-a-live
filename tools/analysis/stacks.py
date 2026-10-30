"""stacks.py LIVE.txt [NAME]: pandora_stacks snapshots (kernel 0317 format) per process and thread.

Prints per process: its images and the shared cache slide, then per thread how often each wait spot
(state, block hint, pc, lr, first returns) shows up over all snapshots. With --addrs prints the unique
return addresses (for symbolication) instead.
"""
import collections
import re
import sys

HINTS = {0: 'none', 1: 'kmutex', 2: 'port-recv', 3: 'portset-recv', 4: 'port-send', 5: 'port-send-transit',
         6: 'semaphore', 7: 'rw-read', 8: 'rw-write', 9: 'rw-upgrade', 10: 'ulock', 11: 'pthread-mutex',
         12: 'pthread-rw-read', 13: 'pthread-rw-write', 14: 'condvar', 15: 'parked-wq', 16: 'workloop-sync',
         17: 'on-process', 18: 'sleep-inheritor', 19: 'eventlink', 20: 'compressor', 21: 'parked-bound-wq'}

ts_rx = re.compile(r'^\[\s*([0-9.]+)\] (.*)$')
hdr_rx = re.compile(r'PANDORA STACKS (\S+)\[(\d+)\]: (\d+) threads, main binary at (0x[0-9a-f]+)')
th_rx = re.compile(r'PANDORA STACKS (\S+)\[(\d+)\] tid (\d+) st ([0-9a-f]+) hint (\d+) pc (0x[0-9a-f]+|0) lr (0x[0-9a-f]+|0)')
ret_rx = re.compile(r'PANDORA STACKS (\S+)\[(\d+)\] tid (\d+) \+(\d+) \|((?: [0-9a-f]+)*)')
img_rx = re.compile(r'PANDORA PROF IMAGE (0x[0-9a-f]+) (\S+)')
cache_rx = re.compile(r'PANDORA PROF IMAGES (\d+), shared cache base (0x[0-9a-f]+) slide (0x[0-9a-f]+)')

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('-') else None
addrs_only = '--addrs' in sys.argv
t_from = t_to = None
for a in sys.argv:
    if a.startswith('--from='):
        t_from = float(a[7:])
    if a.startswith('--to='):
        t_to = float(a[5:])

snaps = collections.defaultdict(list)        # pid -> [(time, {tid: rec})]
cur = {}                                     # pid -> current snapshot dict
images = collections.defaultdict(dict)       # pid -> {addr: path}
slide = {}
names = {}
last_hdr_pid = None
for raw in open(path, 'rb'):
    line = raw.decode('utf-8', 'replace').rstrip('\r\n')
    m = ts_rx.match(line)
    if not m:
        continue
    t, body = float(m.group(1)), m.group(2)
    if t_from is not None and t < t_from or t_to is not None and t > t_to:
        continue
    h = hdr_rx.search(body)
    if h:
        pid = int(h.group(2))
        names[pid] = h.group(1)
        cur[pid] = {}
        snaps[pid].append((t, cur[pid]))
        last_hdr_pid = pid
        continue
    c = cache_rx.search(body)
    if c and last_hdr_pid is not None:
        slide[last_hdr_pid] = int(c.group(3), 16)
        continue
    i = img_rx.search(body)
    if i and last_hdr_pid is not None:
        images[last_hdr_pid][int(i.group(1), 16)] = i.group(2)
        continue
    r = th_rx.search(body)
    if r:
        pid, tid = int(r.group(2)), int(r.group(3))
        if pid in cur:
            cur[pid][tid] = {'st': int(r.group(4), 16), 'hint': int(r.group(5)), 'pc': int(r.group(6), 16),
                             'lr': int(r.group(7), 16), 'ret': []}
        continue
    r = ret_rx.search(body)
    if r:
        pid, tid = int(r.group(2)), int(r.group(3))
        if pid in cur and tid in cur[pid]:
            cur[pid][tid]['ret'] += [int(x, 16) for x in r.group(5).split()]

if addrs_only:
    out = set()
    for pid, ss in snaps.items():
        if want and names[pid] != want:
            continue
        for t, s in ss:
            for rec in s.values():
                out.update(a for a in [rec['pc'], rec['lr']] + rec['ret'] if a)
    for a in sorted(out):
        print('%x' % a)
    sys.exit(0)

for pid, ss in sorted(snaps.items()):
    if want and names[pid] != want:
        continue
    print('==== %s[%d]: %d snapshots %.1f..%.1f s, slide %#x' % (names[pid], pid, len(ss), ss[0][0], ss[-1][0],
                                                               slide.get(pid, 0)))
    for a, p in sorted(images[pid].items()):
        print('  image %#x %s' % (a, p))
    per_tid = collections.defaultdict(collections.Counter)
    seen_in = collections.Counter()
    for t, s in ss:
        for tid, rec in s.items():
            seen_in[tid] += 1
            st = rec['st']
            state = 'RUN' if st & 4 else ('wait' if st & 1 else 'st%x' % st)
            key = '%s %-12s pc %x lr %x | %s' % (state, HINTS.get(rec['hint'], str(rec['hint'])), rec['pc'], rec['lr'],
                                                ' '.join('%x' % x for x in rec['ret'][:6]))
            per_tid[tid][key] += 1
    full = {}
    for t, s in ss:
        for tid, rec in s.items():
            st = rec['st']
            state = 'RUN' if st & 4 else ('wait' if st & 1 else 'st%x' % st)
            key = '%s %-12s pc %x lr %x | %s' % (state, HINTS.get(rec['hint'], str(rec['hint'])), rec['pc'], rec['lr'],
                                                ' '.join('%x' % x for x in rec['ret'][:6]))
            full.setdefault((tid, key), rec)
    for tid in sorted(per_tid, key=lambda x: -seen_in[x]):
        print('  -- tid %d (in %d snapshots)' % (tid, seen_in[tid]))
        for key, n in per_tid[tid].most_common(6):
            print('    %4d %s' % (n, key))
            if '--full' in sys.argv:
                rec = full[(tid, key)]
                for a in [rec['pc'], rec['lr']] + rec['ret']:
                    print('           @ %x' % a)
