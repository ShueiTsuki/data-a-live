"""ul_top.py JSONL PROC [N]: the N most frequent messages of process PROC (numbers folded), with subsystem/category."""
import collections
import json
import re
import sys

proc = sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 20
seen = collections.Counter()
first = {}
with open(sys.argv[1], 'rb') as f:
    for raw in f:
        if proc.encode() not in raw:
            continue
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        if e.get('process', '').split('/')[-1] != proc:
            continue
        m = e.get('message', '').strip().replace('\n', ' ')
        key = (e.get('subsystem', ''), e.get('category', ''), re.sub(r'0x[0-9a-fA-F]+|\d+', 'N', m)[:200])
        seen[key] += 1
        first.setdefault(key, (e.get('pid'), e.get('library', '').split('/')[-1], m[:300]))
total = sum(seen.values())
print('%s: %d messages' % (proc, total))
for key, c in seen.most_common(n):
    pid, lib, m = first[key]
    print('%7d pid %s %s/%s [%s]: %s' % (c, pid, key[0], key[1], lib, m))
