#!/usr/bin/env python3
"""spvio.py <frag.spvasm>: which input attachments a fragment module reads and which colour outputs it stores."""
import re, sys
t = open(sys.argv[1]).read()
loc = dict(re.findall(r'OpDecorate (%\d+) Location (\d+)', t))
iai = dict(re.findall(r'OpDecorate (%\d+) InputAttachmentIndex (\d+)', t))
outs = set(re.findall(r'(%\d+) = OpVariable %\d+ Output', t))
stored = set(re.findall(r'OpStore (%\d+) ', t))
written = sorted(int(loc[v]) for v in outs if v in loc and v in stored)
consts = {}
for v, l in [(v, loc[v]) for v in outs if v in loc and v in stored]:
    m = re.findall(r'OpStore %s (%%\d+)' % v, t)
    consts[l] = m
reads = sorted(int(i) for i in iai.values())
zero = [l for l, vals in consts.items() if any(re.search(r'%s = OpConstant(Null|Composite %%\d+ (%%\d+ ?)+)$' % x, t, re.M) for x in vals)]
print('reads input attachments', reads or 'none', '| writes colors', written or 'none', '| constant-valued writes', sorted(zero) or 'none')
