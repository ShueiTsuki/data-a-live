#!/usr/bin/env python3
"""List a folder of the phone's iOS root, read-only, with the phone in Android.

  ios_ls.py /ABS/FOLDER [DEPTH]

Each entry under FOLDER down to DEPTH levels (default 3): "d" for a folder with its item count, "f" for a file with
its size. Reads the image as tools/ioslog_fetch.py does. From Git Bash pass MSYS_NO_PATHCONV=1.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ioslog_fetch import HFS, Remote  # noqa: E402


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    fs = HFS(Remote())
    kids = {}
    for (parent, name), v in fs.walk().items():
        kids.setdefault(parent, {})[name] = v
    cur = 2                                        # the root folder's CNID
    for part in [p for p in path.split('/') if p]:
        v = kids.get(cur, {}).get(part)
        if v is None or v[0] != 1:
            sys.exit(f'no folder {part} in {path}')
        cur = v[1]

    def show(cnid, prefix, level):
        for name, v in sorted(kids.get(cnid, {}).items()):
            if v[0] == 1:
                print(f'{prefix}d {name}/ ({len(kids.get(v[1], {}))} items)')
                if level < depth:
                    show(v[1], prefix + '  ', level + 1)
            else:
                size = v[3][0][0]
                print(f'{prefix}f {name} {size}')

    show(cur, '', 1)


if __name__ == '__main__':
    main()
