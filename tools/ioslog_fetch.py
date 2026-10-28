#!/usr/bin/env python3
"""Fetch the iOS unified logs of the last boot from the phone (in Android) without copying the whole root image.

  ioslog_fetch.py NAME [--minutes 30] [--parse]

The iOS root is the file /data/local/tmp/iosroot.img on Android's userdata; iOS writes its blocks below Android's
file encryption, so it is read raw (tools/pinfile rawreadat, read-only). This reads the HFS+ volume header, the
catalog and extents-overflow files, finds /private/var/db/diagnostics and /private/var/db/uuidtext, and copies:
  - the trace files of Persist, Special, Signpost and HighVolume changed in the last MINUTES (the test boot), all of
    timesync and the top-level files, into /opt/work/la_NAME (WSL);
  - uuidtext into a cache (/opt/work/ul_cache/uuidtext), only files not there yet or changed, then into la_NAME.
With --parse it runs unifiedlog_iterator to /opt/work/ul_NAME.jsonl. Device logs: they stay outside the repo.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

ADB = 'D:/Android/platform-tools/adb.exe'
IMG = '/data/local/tmp/iosroot.img'
WSL = '//wsl$/Ubuntu-24.04'
BLK = 4096
HFS_EPOCH = 2082844800            # seconds from 1904-01-01 to 1970-01-01


class Remote:
    """Raw reads of the image file through pinfile, in whole 4 KiB blocks, with merged ranges and a cache."""

    def __init__(self):
        out = subprocess.run([ADB, 'shell', "su -c 'readlink -f /dev/block/by-name/userdata'"], capture_output=True,
                             text=True, env=dict(os.environ, MSYS_NO_PATHCONV='1')).stdout.strip()
        if not out.startswith('/dev/'):
            raise SystemExit(f'no userdata device: {out!r}')
        self.dev = out
        self.calls = 0
        self.bytes = 0

    def read(self, off, length):
        a = off - off % BLK
        b = off + length
        b += (-b) % BLK
        cmd = f"su -c '/data/local/tmp/pinfile rawreadat {IMG} {self.dev} {a} {b - a} 2>/dev/null'"
        data = subprocess.run([ADB, 'exec-out', cmd], capture_output=True,
                              env=dict(os.environ, MSYS_NO_PATHCONV='1')).stdout
        self.calls += 1
        self.bytes += len(data)
        if len(data) != b - a:
            raise SystemExit(f'short read at {a}: {len(data)} of {b - a}')
        return data[off - a: off - a + length]


def be(fmt, b, o=0):
    return struct.unpack_from('>' + fmt, b, o)


class HFS:
    def __init__(self, r):
        self.r = r
        vh = r.read(0, BLK)[1024:1536]
        sig = vh[:2]
        if sig not in (b'H+', b'HX'):
            raise SystemExit(f'not HFS+: {sig!r}')
        self.bs = be('I', vh, 0x28)[0]
        self.ext_fork = self.fork(vh, 0xC0)
        self.cat_fork = self.fork(vh, 0x110)
        self.overflow = {}
        ext_file = self.read_fork(self.ext_fork, 3)
        self.parse_extents(ext_file)
        self.catalog = self.read_fork(self.cat_fork, 4)

    @staticmethod
    def fork(b, o):
        size, clump, blocks = be('QII', b, o)
        exts = [be('II', b, o + 16 + 8 * i) for i in range(8)]
        return size, blocks, [e for e in exts if e[1]]

    def extents_of(self, fid, fork):
        size, blocks, exts = fork
        exts = list(exts)
        have = sum(n for _, n in exts)
        while have < blocks:
            more = self.overflow.get((fid, have))
            if more is None:
                raise SystemExit(f'file {fid}: extents missing at block {have}')
            exts += more
            have += sum(n for _, n in more)
        return size, exts

    def read_fork(self, fork, fid=None, cache=None):
        size, exts = self.extents_of(fid, fork) if fid is not None else (fork[0], fork[2])
        out = bytearray()
        for start, n in exts:
            want = min(n * self.bs, size - len(out))
            if want <= 0:
                break
            step = 32 << 20
            for at in range(0, want, step):
                out += self.r.read(start * self.bs + at, min(step, want - at))
        return bytes(out[:size])

    def nodes(self, tree):
        ns = be('H', tree, 14 + 18)[0]
        first = be('I', tree, 14 + 10)[0]
        n = first
        while n:
            node = tree[n * ns:(n + 1) * ns]
            flink, blink, kind, height, nrec = be('IIbBH', node)
            offs = [be('H', node, ns - 2 * (i + 1))[0] for i in range(nrec + 1)]
            yield node, offs, nrec
            n = flink

    def parse_extents(self, tree):
        if not tree:
            return
        for node, offs, nrec in self.nodes(tree):
            for i in range(nrec):
                o = offs[i]
                klen, ftype, pad, fid, start = be('HBBII', node, o)
                if ftype != 0:
                    continue
                rec = o + 2 + klen
                self.overflow[(fid, start)] = [e for e in (be('II', node, rec + 8 * j) for j in range(8)) if e[1]]

    def walk(self):
        """(parentID, name) -> (kind, id, mtime, fork) for every folder (kind 1) and file (kind 2)."""
        ents = {}
        for node, offs, nrec in self.nodes(self.catalog):
            for i in range(nrec):
                o = offs[i]
                klen, parent, nlen = be('HIH', node, o)
                name = node[o + 8: o + 8 + 2 * nlen].decode('utf-16-be', 'replace')
                rec = o + 2 + klen
                rtype = be('h', node, rec)[0]
                if rtype == 1:
                    ents[(parent, name)] = (1, be('I', node, rec + 8)[0], be('I', node, rec + 16)[0], None)
                elif rtype == 2:
                    fid, mtime = be('I', node, rec + 8)[0], be('I', node, rec + 16)[0]
                    flags = node[rec + 32 + 9]          # permissions.ownerFlags
                    ents[(parent, name)] = (2, fid, mtime, (self.fork(node, rec + 88), flags))
        return ents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--minutes', type=int, default=30)
    ap.add_argument('--parse', action='store_true')
    a = ap.parse_args()
    t0 = time.time()
    r = Remote()
    fs = HFS(r)
    ents = fs.walk()
    kids = {}
    for (parent, name), v in ents.items():
        kids.setdefault(parent, {})[name] = v

    def lookup(path):
        cur = 2                                    # the root folder's CNID
        for part in path.strip('/').split('/'):
            v = kids.get(cur, {}).get(part)
            if v is None or v[0] != 1:
                raise SystemExit(f'no folder {path} ({part})')
            cur = v[1]
        return cur

    def files_under(fid, rel=''):
        for name, v in kids.get(fid, {}).items():
            if v[0] == 1:
                yield from files_under(v[1], rel + name + '/')
            else:
                yield rel + name, v

    diag = lookup('private/var/db/diagnostics')
    uuid = lookup('private/var/db/uuidtext')
    newest = max(v[2] for _, v in files_under(diag))
    since = newest - 60 * a.minutes
    la = f'{WSL}/opt/work/la_{a.name}'
    cache = f'{WSL}/opt/work/ul_cache/uuidtext'
    shutil.rmtree(la, ignore_errors=True)
    os.makedirs(la)
    got = skipped = 0

    def fetch(v, dst):
        (fork, flags) = v[3]
        if flags & 0x20:                           # UF_COMPRESSED: data in the decmpfs attribute
            return False
        data = fs.read_fork(fork, v[1])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        return True

    for rel, v in files_under(diag):
        top = rel.split('/')[0]
        if '/' in rel and top in ('Persist', 'Special', 'Signpost', 'HighVolume') and v[2] < since:
            continue
        if fetch(v, f'{la}/{rel}'):
            got += 1
        else:
            skipped += 1
    nu = nnew = 0
    for rel, v in files_under(uuid):
        dst = f'{cache}/{rel}'
        size = v[3][0][0]
        if not (os.path.exists(dst) and os.path.getsize(dst) == size):
            if not fetch(v, dst):
                skipped += 1
                continue
            nnew += 1
        os.makedirs(os.path.dirname(f'{la}/{rel}'), exist_ok=True)
        shutil.copyfile(dst, f'{la}/{rel}')
        nu += 1
    shutil.copyfile(f'{WSL}/opt/work/tools/ul_version.plist', f'{la}/version.plist')
    print(f'diagnostics: {got} files (trace files since {time.strftime("%H:%M", time.gmtime(since - HFS_EPOCH))} UTC); '
          f'uuidtext: {nu} files, {nnew} new; {skipped} compressed skipped; '
          f'{r.calls} reads, {r.bytes >> 20} MiB, {time.time() - t0:.0f} s')
    if a.parse:
        cmd = (f'/opt/work/macos-UnifiedLogs/examples/target/release/unifiedlog_iterator -m log-archive '
               f'-i /opt/work/la_{a.name} -o /opt/work/ul_{a.name}.jsonl -f jsonl > /tmp/ulit_{a.name}.log 2>&1; '
               f'ls -la /opt/work/ul_{a.name}.jsonl')
        subprocess.run(['wsl', '-d', 'Ubuntu-24.04', '-u', 'root', '-e', 'bash', '-c', cmd])


if __name__ == '__main__':
    main()
