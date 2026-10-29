#!/usr/bin/env python3
"""Scale iOS's per-device memory limits to this phone's memory.

  jetsam_scale.py SRC_PLIST OUT_PLIST [--factor 4]

SRC_PLIST is an iPhone X jetsam configuration from the IPSW (com.apple.jetsamproperties.D22.plist or D221).
Every value whose key ends in "MemoryLimit" and is above 0 (megabytes; -1 means no limit) is multiplied by
FACTOR; the rest stays as it is, in the file's own format. The iPhone X has 3 GB of memory, this phone 15 GB: at
the iPhone X limits the Astronomy poster (PosterProvider_Rendering_EntitledSoftMemoryLimit, 200 MB) had no room
left once the GPU translation's memory was counted (post6). Apple's file: keep OUT_PLIST out of git.
"""
import argparse
import plistlib

ap = argparse.ArgumentParser()
ap.add_argument('src')
ap.add_argument('out')
ap.add_argument('--factor', type=int, default=4)
args = ap.parse_args()

raw = open(args.src, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
data = plistlib.loads(raw)
changed = []


def scale(node, path):
    if isinstance(node, dict):
        for key, value in node.items():
            here = path + '/' + key
            if key.endswith('MemoryLimit') and isinstance(value, int) and not isinstance(value, bool) and value > 0:
                node[key] = value * args.factor
                changed.append((here, value, node[key]))
            else:
                scale(value, here)
    elif isinstance(node, list):
        for i, value in enumerate(node):
            scale(value, '%s[%d]' % (path, i))


scale(data, '')
open(args.out, 'wb').write(plistlib.dumps(data, fmt=fmt))
print('%s: %d limits x%d' % (args.out, len(changed), args.factor))
for here, old, new in changed:
    if 'Poster' in here:
        print('  %s %d -> %d' % (here, old, new))
