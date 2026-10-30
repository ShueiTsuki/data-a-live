"""ul_grep.py JSONL OUT REGEX: every message of the last boot (newest entry's boot_uuid) whose process or text matches
REGEX, written to OUT as 'seconds-since-boot process[pid] type: text' lines in time order (run in WSL)."""
import collections
import json
import re
import sys

path, out, pat = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
rows, first = collections.defaultdict(list), {}
newest, boot = -1, None
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
    p = (d.get('process') or '').split('/')[-1]
    m = (d.get('message') or '').replace('\n', ' ')
    if pat.search(p) or pat.search(m):
        rows[b].append((t, '%s[%s] %s: %s' % (p, d.get('pid'), d.get('log_type') or '', m[:400])))
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    for t, s in sorted(rows[boot]):
        f.write('%8.2f %s\n' % ((t - first[boot]) / 1e9 if t > 1e12 else t - first[boot], s))
print(len(rows[boot]), 'lines')
