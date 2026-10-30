"""ul_seq.py JSONL PROC SKIP COUNT [PROC2...]: COUNT consecutive messages of PROC (and PROC2...) after skipping SKIP of
PROC's, in log order, with thread, subsystem/category and full text (a loop's one turn)."""
import json
import sys

procs = set([sys.argv[2]] + sys.argv[5:])
skip, count = int(sys.argv[3]), int(sys.argv[4])
seen = 0
out = 0
with open(sys.argv[1], 'rb') as f:
    for raw in f:
        if not any(p.encode() in raw for p in procs):
            continue
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        p = e.get('process', '').split('/')[-1]
        if p not in procs:
            continue
        if p == sys.argv[2]:
            seen += 1
        if seen <= skip:
            continue
        print('%.6f %s[%s] t%s %s/%s %s: %s' % (e['time'] / 1e9 % 1000, p, e.get('pid'), e.get('thread_id'),
                                              e.get('subsystem', ''), e.get('category', ''), e.get('event_type', ''),
                                              e.get('message', '').replace('\n', ' ')[:400]))
        out += 1
        if out >= count:
            break
