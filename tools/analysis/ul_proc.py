"""ul_proc.py JSONL PROCESS [REGEX] [--from SECONDS]: messages of PROCESS in the last boot (the newest entry's
boot_uuid), in time order with seconds since that boot's first entry, distinct texts once each with their count
(run in WSL)."""
import collections
import json
import re
import sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
path, proc = args[0], args[1]
pat = re.compile(args[2]) if len(args) > 2 else None
start = 0.0
if '--from' in sys.argv:
    start = float(sys.argv[sys.argv.index('--from') + 1])
rows, newest, boot = [], -1, None
first = {}
for line in open(path, encoding='utf-8', errors='replace'):
    try:
        d = json.loads(line)
    except ValueError:
        continue
    t, b = d.get('time') or 0, d.get('boot_uuid')
    if t > newest:
        newest, boot = t, b
    if b not in first or t < first[b]:
        first[b] = t
    if (d.get('process') or '').split('/')[-1] != proc:
        continue
    m = (d.get('message') or '').replace('\n', ' ')
    if pat is None or pat.search(m):
        rows.append((t, b, m))
t0 = first.get(boot, 0)
seen = collections.OrderedDict()
for t, b, m in sorted(r for r in rows if r[1] == boot):
    s = (t - t0) / 1e9
    if s < start:
        continue
    k = m[:200]
    if k in seen:
        seen[k][1] += 1
    else:
        seen[k] = [s, 1]
for k, (s, n) in list(seen.items())[:80]:
    print(f'{s:8.2f} x{n} {k}')
