#!/usr/bin/env python3
"""Copy one file of the phone's iOS root out, read-only, with the phone in Android.

  ios_cat.py /ABS/PATH OUTFILE

Reads the iOS root image the way tools/ioslog_fetch.py reads the logs (pinfile rawreadat). From Git Bash pass
MSYS_NO_PATHCONV=1, or the path turns into a Windows one. Device data: OUTFILE outside the repo (device/).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ioslog_fetch import HFS, Remote  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, out = sys.argv[1], sys.argv[2]
    fs = HFS(Remote())
    kids = {}
    for (parent, name), v in fs.walk().items():
        kids.setdefault(parent, {})[name] = v
    cur, v = 2, None                               # the root folder's CNID
    parts = [p for p in path.split('/') if p]
    for i, part in enumerate(parts):
        v = kids.get(cur, {}).get(part)
        if v is None:
            sys.exit(f'not found: {part} in /{"/".join(parts[:i])}')
        if i < len(parts) - 1:
            if v[0] != 1:
                sys.exit(f'not a folder: {part}')
            cur = v[1]
    if v is None or v[0] != 2:
        sys.exit('not a file')
    data = fs.read_fork(v[3][0], v[1])
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(data)
    print(f'{path}: {len(data)} bytes -> {out}')


if __name__ == '__main__':
    main()
