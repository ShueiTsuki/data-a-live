#!/usr/bin/env python3
"""Compare the iOS root on the phone with a local HFS+ image, by catalog: added, removed and changed entries.

  iosroot_diff.py BASE_IMAGE [--all-var]

Reads the phone's image read-only through tools/pinfile rawreadat (tools/ioslog_fetch.py's Remote and HFS) and
BASE_IMAGE (a WSL path such as /opt/work/iosroot/ios-root-base.img) locally. A file is changed when its size or
content modification date differs. /private/var changes all the time (logs, caches, databases); without --all-var
only its .plist files and folders are listed.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ioslog_fetch as lf  # noqa: E402

WSL = '//wsl$/Ubuntu-24.04'


class Local:
    def __init__(self, path):
        self.f = open(WSL + path if path.startswith('/') else path, 'rb')
        self.calls = 0
        self.bytes = 0

    def read(self, off, length):
        self.f.seek(off)
        self.calls += 1
        self.bytes += length
        return self.f.read(length)


def paths(fs):
    ents = fs.walk()
    kids = {}
    for (parent, name), v in ents.items():
        kids.setdefault(parent, []).append((name, v))
    out = {}
    stack = [(2, '')]
    while stack:
        fid, pre = stack.pop()
        for name, v in kids.get(fid, []):
            p = pre + '/' + name
            size = v[3][0][0] if v[0] == 2 else None
            out[p] = (v[0], size, v[2])
            if v[0] == 1:
                stack.append((v[1], p))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('--all-var', action='store_true')
    a = ap.parse_args()
    phone = paths(lf.HFS(lf.Remote()))
    base = paths(lf.HFS(Local(a.base)))

    def keep(p):
        if not p.startswith('/private/var/') or a.all_var:
            return True
        return p.endswith('.plist')

    def when(t):
        return time.strftime('%Y-%m-%d %H:%M', time.gmtime(t - lf.HFS_EPOCH)) if t else '-'

    added = sorted(p for p in phone if p not in base and keep(p))
    removed = sorted(p for p in base if p not in phone and keep(p))
    changed = sorted(p for p in phone if p in base and phone[p][0] == 2 and keep(p) and
                     (phone[p][1] != base[p][1] or phone[p][2] != base[p][2]))
    # collapse added/removed subtrees to their top entry
    def tops(lst):
        out = []
        for p in lst:
            if out and p.startswith(out[-1] + '/'):
                continue
            out.append(p)
        return out
    print(f'phone {len(phone)} entries, base {len(base)}')
    print(f'== added ({len(added)} entries, {len(tops(added))} top-level)')
    for p in tops(added):
        print(f'  + {p}  {when(phone[p][2])}')
    print(f'== removed ({len(removed)} entries, {len(tops(removed))} top-level)')
    for p in tops(removed):
        print(f'  - {p}')
    print(f'== changed files ({len(changed)})')
    for p in changed:
        print(f'  * {p}  size {base[p][1]} -> {phone[p][1]}  {when(base[p][2])} -> {when(phone[p][2])}')


if __name__ == '__main__':
    main()
