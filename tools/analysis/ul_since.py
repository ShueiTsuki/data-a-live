"""ul_since.py JSONL ANCHOR SECONDS PROC [REGEX]: messages of process PROC (basename) after the boot that logged ANCHOR.

ANCHOR is a kernel message text logged SECONDS after boot (e.g. "PANDORA SHOT: 1 at" with 97.9): the boot's start is
that message's time minus SECONDS. Prints counts of messages (numbers folded), errors and faults first, with the
first message's seconds after boot.
"""
import collections
import json
import re
import sys

path, anchor, secs, proc = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
rx = re.compile(sys.argv[5], re.I) if len(sys.argv) > 5 else None
boot = None
with open(path, 'rb') as f:
    for raw in f:
        if anchor.encode() in raw:
            e = json.loads(raw)
            if anchor in e.get('message', ''):
                boot = e['time'] / 1e9 - secs
if boot is None:
    sys.exit('anchor not found')
print('boot at epoch %.1f' % boot)
seen = collections.Counter()
first = {}
kind = {}
pids = collections.Counter()
with open(path, 'rb') as f:
    for raw in f:
        if proc.encode() not in raw:
            continue
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        t = e['time'] / 1e9 - boot
        if t < 0 or e.get('process', '').split('/')[-1] != proc:
            continue
        m = e.get('message', '').strip().replace('\n', ' ')
        if rx and not rx.search(m) and not rx.search(e.get('subsystem', '')):
            continue
        key = re.sub(r'0x[0-9a-fA-F]+|\d+', 'N', m)[:240]
        seen[key] += 1
        pids[e.get('pid')] += 1
        if key not in first:
            first[key] = (t, e.get('pid'), m[:420])
            kind[key] = e.get('log_type', '')
print('pids', dict(pids))
order = sorted(seen, key=lambda k: (kind[k] not in ('Error', 'Fault'), first[k][0]))
for k in order[:int(sys.argv[6]) if len(sys.argv) > 6 else 80]:
    t, pid, m = first[k]
    print('%5d %-7s %6.1fs pid %s: %s' % (seen[k], kind[k], t, pid, m))
