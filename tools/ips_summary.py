#!/usr/bin/env python3
"""Summarise iOS crash reports (.ips, the JSON format of iOS 15+): the exception, the termination reason, the
last exception backtrace or the crashed thread's frames with their images and symbols.

  ips_summary.py FILE.ips... [--frames N]

Device data: the files stay outside the repo (device/).
"""
import json
import sys


def main():
    files = [a for a in sys.argv[1:] if not a.startswith('--')]
    n = 14
    for a in sys.argv[1:]:
        if a.startswith('--frames='):
            n = int(a.split('=', 1)[1])
    for path in files:
        text = open(path, encoding='utf-8', errors='replace').read()
        head, _, body = text.partition('\n')
        try:
            meta, rep = json.loads(head), json.loads(body)
        except ValueError:
            print(f'== {path}: not an .ips JSON report ({len(text)} bytes)')
            continue
        images = rep.get('usedImages', [])
        print(f'== {path}: {meta.get("app_name") or rep.get("procName")} {meta.get("timestamp", "")}')
        exc = rep.get('exception', {})
        print(f'   exception {exc.get("type")} {exc.get("signal", "")} {exc.get("subtype", "")}'.rstrip())
        term = rep.get('termination', {})
        if term:
            print(f'   termination {term.get("namespace")} {term.get("code")}: {term.get("indicator", "")}')
            for d in term.get('details', [])[:3]:
                print(f'     {d[:300]}')
        asi = rep.get('asi')
        if asi:
            for k, v in asi.items():
                for line in v[:3]:
                    print(f'   asi {k}: {line[:300]}')
        frames = rep.get('lastExceptionBacktrace')
        label = 'last exception backtrace'
        if not frames:
            ct = rep.get('faultingThread', 0)
            threads = rep.get('threads', [])
            frames = threads[ct].get('frames', []) if ct < len(threads) else []
            label = f'thread {ct}' + (f' ({threads[ct].get("queue")})' if ct < len(threads) and threads[ct].get('queue') else '')
        print(f'   {label}:')
        for fr in frames[:n]:
            img = images[fr['imageIndex']] if fr.get('imageIndex', -1) < len(images) else {}
            name = img.get('name') or (img.get('path') or '?').split('/')[-1]
            sym = fr.get('symbol', '')
            off = fr.get('symbolLocation', fr.get('imageOffset'))
            print(f'     {name} {sym}+{off}' if sym else f'     {name} +{fr.get("imageOffset")}')


if __name__ == '__main__':
    main()
