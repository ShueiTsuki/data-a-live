"""chain_counts.py DUMP...: for each A11 driver class, how many instance methods each class of its chain declares."""
import re
import sys

cls = {}
for path in sys.argv[1:]:
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^@interface (\w+)(?: : (\w+))?', line)
        if m and not re.match(r'^@interface \w+ \(', line):
            cur = cls.setdefault(m.group(1), {'super': m.group(2), 'n': 0})
            continue
        if line.startswith('@end'):
            cur = None
            continue
        if cur is not None and line.startswith('-['):
            cur['n'] += 1
for start in ('AGXA11Device', 'AGXA11FamilyCommandQueue', 'AGXA11FamilyCommandBuffer', 'AGXA11FamilyRenderContext',
              'AGXA11FamilyComputeContext', 'AGXA11FamilyBlitContext', 'AGXA11FamilyTexture', 'AGXA11FamilyBuffer',
              'AGXA11FamilyRenderPipeline', 'AGXA11FamilyHeap'):
    c, parts = start, []
    while c and c in cls:
        parts.append('%s %d' % (c, cls[c]['n']))
        c = cls[c]['super']
    print(' <- '.join(parts))
