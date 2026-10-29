#!/usr/bin/env python3
"""timeline.py CAPDIR: one time-ordered list of what Android did to power the video core up and down
(capture_android.sh with REGMAP=1): the video driver's kernel log (msm_vidc lines, register reads and writes of the
core itself), clock and power-domain events and the clock controllers' register reads and writes (regmap), merged
by time (ftrace's "local" clock is the kernel log's). Repeated identical reads in a row (polls) become one line
with a count.

Writes CAPDIR/timeline_on.txt (from the driver's resume or open to "boot firmware is successful") and
CAPDIR/timeline_off.txt (from "__power_off_iris4" to "power collapse successful"), the first of each.
"""
import re
import sys
from pathlib import Path

cap = Path(sys.argv[1])
CONTROLLERS = {'20f0000.clock-controller': 'videocc', '100000.clock-controller': 'gcc'}

rows = []
traced_from = None              # dmesg -w replays the older log first: windows start after the trace does
dm = re.compile(r'^\[\s*(\d+\.\d+)\] msm_vidc: \w+\s*: [0-9a-f]*: ?[^:]*: (.*)$')
for line in (cap / 'dmesg.txt').read_text(errors='replace').splitlines():
    m = dm.match(line)
    if m:
        rows.append((float(m.group(1)), 'vidc', m.group(2).strip()))

tr = re.compile(r'\s(\d+\.\d+): (clk_\w+|power_domain_target|icc_set_bw|regmap_reg_write|regmap_reg_read|rpm_\w+): (.*)$')
for line in (cap / 'trace.txt').read_text(errors='replace').splitlines():
    m = tr.search(line)
    if not m:
        continue
    t, ev, rest = float(m.group(1)), m.group(2), m.group(3)
    traced_from = t if traced_from is None else min(traced_from, t)
    if ev.startswith('regmap'):
        dev, _, regval = rest.partition(' ')
        if dev not in CONTROLLERS:
            continue
        op = 'W' if ev.endswith('write') else 'R'
        rows.append((t, CONTROLLERS[dev], '%s %s' % (op, regval.strip())))
    elif ev.startswith('clk') and 'video' not in rest:
        continue
    else:
        rows.append((t, ev, rest.strip()))
rows.sort(key=lambda r: r[0])


def window(start_rx, end_rx):
    out, on = [], False
    for t, src, text in rows:
        if not on and src == 'vidc' and t >= (traced_from or 0) and re.search(start_rx, text):
            on = True
        if on:
            out.append((t, src, text))
            if src == 'vidc' and re.search(end_rx, text):
                break
    return out


def squeeze(win):
    out = []
    for t, src, text in win:
        if out and out[-1][1] == src and out[-1][2] == text and text.startswith('R '):
            out[-1][3] += 1
            continue
        out.append([t, src, text, 1])
    return ['%.6f %-8s %s%s' % (t, src, text, ' (x%d)' % n if n > 1 else '') for t, src, text, n in out]


for name, a, b in (('on', r'Resuming from power collapse|msm_vidc_open: Video-Banner', r'boot firmware is successful'),
                   ('off', r'__power_off_iris4', r'power collapse successful')):
    lines = squeeze(window(a, b))
    (cap / ('timeline_%s.txt' % name)).write_text('\n'.join(lines) + '\n')
    print(name, len(lines), 'lines')
