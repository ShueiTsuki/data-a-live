"""ul_lastboot.py JSONL PATTERN...: count messages matching each regex PATTERN in the last boot only (the boot_uuid of
the newest entry), grouped by process and message start (run in WSL)."""
import collections
import json
import re
import sys

path, pats = sys.argv[1], [re.compile(p) for p in sys.argv[2:]]
rows, newest, boot = [], -1, None
for line in open(path, encoding='utf-8', errors='replace'):
    try:
        d = json.loads(line)
    except ValueError:
        continue
    t = d.get('time') or 0
    if t > newest:
        newest, boot = t, d.get('boot_uuid')
    m = (d.get('message') or '').replace('\n', ' ')
    if any(p.search(m) for p in pats):
        rows.append((d.get('boot_uuid'), (d.get('process') or '').split('/')[-1], m))
c = collections.Counter((p, m[:200]) for b, p, m in rows if b == boot and p != 'kernel')
print('boot', boot, 'matches', sum(c.values()))
for (p, m), n in c.most_common(25):
    print(n, p, m)
