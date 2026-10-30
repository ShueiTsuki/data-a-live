#!/usr/bin/env python3
"""pass_shaders.py <pass spec> <qcair dir>: per draw, what Apple's specialized fragment function reads and returns."""
import re, subprocess, sys, os
spec, air = sys.argv[1], sys.argv[2]
blocks, cur = [], None
for line in open(spec):
    line = line.split('  #')[0].rstrip()
    if line.startswith('begin '):
        cur = {'label': line.split()[1], 'f': None, 'fc': [], 'colors': []}
        blocks.append(cur)
    elif cur is not None:
        if line.startswith('fragment '):
            cur['f'] = line.split()[1]
        elif line.startswith('fconst '):
            cur['fc'] += line.split()[1:]
        elif line.startswith('color '):
            cur['colors'].append(' '.join(line.split()[1:]))
mods = {}
for f in sorted(os.listdir(air)):
    if f.endswith('.ll') and f.startswith('m'):
        for name in re.findall(r'^define [^@]*@(\w+)\(', open(os.path.join(air, f)).read(), re.M):
            mods.setdefault(name, os.path.join(air, f))
os.makedirs(os.path.join(air, 'spec'), exist_ok=True)
for b in blocks:
    mod = mods.get(b['f'])
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'air_specialize.py'), mod, b['f'],
                        os.path.join(air, 'spec', 'pass_%s.ll' % b['label'])] + b['fc'], capture_output=True, text=True)
    info = ' '.join(l for l in r.stdout.splitlines() if l.startswith(('reads', 'returns'))) or r.stderr.strip()[-200:]
    print('%-4s %-26s %-44s %s' % (b['label'], b['f'], ' ; '.join(b['colors']), info))
