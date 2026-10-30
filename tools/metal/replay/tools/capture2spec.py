#!/usr/bin/env python3
"""capture2spec.py <bbNN-replay.txt> <library> [--draws 0,1,2,3] [--keep-bound-offsets] [--texfill 'fill=...']

Turns draws of the plugin's reassembled capture (`== draw N` / `== draw dN` sections of
`key = value` lines) into one m2v-replay pass spec, in the order given by --draws (GPU order; the
sections are in first-appearance order). Without --draws: the first section and every later one
whose pass has the same c0 texture, in section order.

Buffers: the commit-time keys (`commit vbuffer2+208 = <avail>: <hex>`, `commit index+480 = ...`)
are used; bind-time `vbufferN`/`fbufferN` bytes only when a draw has no commit key for that binding
(marked "stale" in the spec). The probe does not see setVertexBufferOffset: for an indexed draw
the vertex buffer 1 offset is taken as index offset - (largest index + 1) * stride, the block
QuartzCore writes just before its indices (--keep-bound-offsets turns that off).

Texture contents are not in the capture: ASTC 4x4 textures get left half red, right half green
void-extent blocks; other textures a solid colour from a fixed palette (named in a comment).
"""
import re
import sys

PALETTE = [('blue', '0 0 1 1'), ('yellow', '1 1 0 1'), ('magenta', '1 0 1 1'), ('cyan', '0 1 1 1'), ('grey', '0.5 0.5 0.5 1')]


def sections(path):
    out, order, cur = {}, [], None
    for line in open(path):
        line = line.rstrip('\n')
        m = re.match(r'== draw (\S+)', line)
        if m:
            cur = m.group(1)
            out[cur] = {}
            order.append(cur)
            continue
        if cur is not None and ' = ' in line and not line.startswith('~'):
            k, v = line.split(' = ', 1)
            out[cur][k.strip()] = v.strip()
    return out, order


def hexbytes(v):
    """`<avail>: <hex groups>` or `len L off O A: <hex>` -> bytes."""
    tail = v.split(': ', 1)[1] if ': ' in v else v
    return bytes.fromhex(''.join(t for t in tail.split() if re.fullmatch(r'[0-9a-fA-F]+', t)))


def parse_pipe(pipe):
    m = re.match(r'v (\S+) \{([^}]*)\} f (\S+) \{([^}]*)\}(.*)', pipe)
    vname, vc, fname, fc, rest = m.groups()
    colors = {}
    for cm in re.finditer(r'c(\d+) (\d+) mask (\d+) blend (\d+)(?: (\d+)/(\d+)/(\d+) (\d+)/(\d+)/(\d+))?', rest):
        i, fmt, mask, blend = cm.group(1, 2, 3, 4)
        f = cm.group(5, 6, 7, 8, 9, 10)
        colors[int(i)] = (fmt, mask, blend, f)
    attrs = re.findall(r'a\d+=[\d:]+', rest)
    sm = re.search(r'samples (\d+)', rest)
    return vname, vc.split(), fname, fc.split(), colors, attrs, sm.group(1) if sm else '1'


def main():
    args = sys.argv[1:]
    opts = {}
    for flag in ('--draws', '--texfill'):
        if flag in args:
            i = args.index(flag)
            opts[flag] = args[i + 1]
            del args[i:i + 2]
    keep_offsets = '--keep-bound-offsets' in args
    args = [a for a in args if a != '--keep-bound-offsets']
    path, lib = args[0], args[1]
    secs, order = sections(path)
    if '--draws' in opts:
        names = opts['--draws'].split(',')
    else:
        first = order[0]
        c0 = re.search(r'\| c0 (\S+)', secs[first]['pass']).group(1)
        names = [n for n in order if re.search(r'\| c0 (\S+)', secs[n].get('pass', '')) and
                 re.search(r'\| c0 (\S+)', secs[n]['pass']).group(1) == c0]
    out = [f'# {len(names)} draws of {path}, in this order: {" ".join(names)}', f'library {lib}']
    d0 = secs[names[0]]
    parts = [p.strip() for p in d0['pass'].split('|')]
    rt = re.match(r'rt (\d+)x(\d+)', parts[0])
    out.append(f'rt {rt.group(1)}x{rt.group(2)}')
    for p in parts[1:]:
        pm = re.match(r'c(\d+) (\S+) (\d+)x(\d+) f(\d+) m(\d+) t(\d+) load(\d) store(\d) clear (\S+)', p)
        i, tid, w, h, fmt, _mips, _ttype, load, store, clear = pm.groups()
        out.append(f'attachment {i} format={fmt} size={w}x{h} load={load} store={store} clear={clear}  # texture {tid}')
    texcolour = {}
    for n in names:
        d = secs[n]
        if d.get('pass') != d0.get('pass'):
            out.append(f'# WARNING draw {n} has another pass: {d.get("pass")}')
        out.append('')
        out.append(f'begin {n}  # pipe {d.get("pipeid", "?")}')
        vname, vc, fname, fc, colors, attrs, samples = parse_pipe(d['pipe'])
        out.append(f'vertex {vname}')
        out.append('vconst ' + ' '.join(vc))
        out.append(f'fragment {fname}')
        for i in range(0, len(fc), 8):
            out.append('fconst ' + ' '.join(fc[i:i + 8]))
        if samples != '1':
            out.append(f'# raster samples {samples} (not modelled by the harness)')
        for i, (fmt, mask, blend, f) in sorted(colors.items()):
            line = f'color {i} format={fmt} mask={mask}'
            if blend != '0' and f[0] is not None:
                line += f' rgb={f[0]},{f[1]},{f[2]} alpha={f[3]},{f[4]},{f[5]}'
            out.append(line)
        if attrs:
            out.append('attr ' + ' '.join(attrs))
        strides = {}
        for a in attrs:
            fmt, off, buf, stride = a.split('=')[1].split(':')[:4]
            strides[int(buf)] = int(stride)
        dm = re.match(r'drawIndexed prim (\d+) count (\d+) type (\d+) offset (\d+) instances (\d+) baseVertex (-?\d+)', d['draw'])
        commits = {}
        for k, v in d.items():
            cm = re.match(r'commit ([vf])buffer(\d+)\+(\d+)$', k)
            if cm:
                commits[(cm.group(1), int(cm.group(2)))] = (int(cm.group(3)), hexbytes(v))
        indices = None
        for k, v in d.items():
            im = re.match(r'commit index\+(\d+)$', k)
            if im:
                indices = (int(im.group(1)), hexbytes(v))
        for k, v in sorted(d.items()):
            bm = re.match(r'([vf])buffer(\d+)$', k)
            if not bm:
                continue
            stage, idx = bm.group(1), int(bm.group(2))
            vm = re.match(r'len (\d+) off (\d+)', v)
            length, off = int(vm.group(1)), int(vm.group(2))
            if (stage, idx) in commits:
                coff, data = commits[(stage, idx)]
                bind, note = coff, 'commit-time'
                if stage == 'v' and idx in strides and dm and indices and not keep_offsets:
                    count = int(dm.group(2))
                    isz = 2 if dm.group(3) == '0' else 4
                    ib = indices[1][:count * isz]
                    top = max(int.from_bytes(ib[j:j + isz], 'little') for j in range(0, len(ib), isz))
                    guess = int(dm.group(4)) - (top + 1) * strides[idx]
                    if guess != coff and coff <= guess and guess + (top + 1) * strides[idx] <= coff + len(data):
                        bind = guess
                        note = f'commit-time; bound at {guess} = index offset {dm.group(4)} - {top + 1} vertices x {strides[idx]} (the probe does not see setVertexBufferOffset; captured offset {coff})'
                out.append(f'buffer {stage} {idx} len={length} offset={bind} data={coff} hex={data.hex()}  # {note}')
            else:
                out.append(f'buffer {stage} {idx} len={length} offset={off} hex={hexbytes(v).hex()}  # bind-time bytes (stale)')
        for k, v in sorted(d.items()):
            sm = re.match(r'([vf])sampler(\d+)$', k)
            if sm:
                vals = dict(re.findall(r'([a-z]+)(\d+(?:/\d+)*)', v))
                a = vals['addr'].split('/')
                out.append(f'sampler {sm.group(1)} {sm.group(2)} min={vals["min"]} mag={vals["mag"]} mip={vals["mip"]} '
                           f's={a[0]} t={a[1] if len(a) > 1 else a[0]} border={vals["border"]} norm={vals["norm"]}')
            bm = re.match(r'([vf])bytes(\d+)$', k)
            if bm:
                out.append(f'bytes {bm.group(1)} {bm.group(2)} {hexbytes(v).hex()}')
            tm = re.match(r'([vf])tex(\d+)$', k)
            if tm:
                tv = re.match(r'(\S+) (\d+)x(\d+) f(\d+)', v)
                tid, w, h, fmt = tv.groups()
                if opts.get('--texfill') and fmt in ('204', '186'):
                    fill, what = opts['--texfill'], 'given fill'
                elif fmt in ('204', '186'):
                    fill, what = f'fill=astc-void-split {(int(w) + 3) // 4 // 2} 1 0 0 1 0 1 0 1', 'left half red, right half green'
                else:
                    name, rgba = texcolour.setdefault(tid, PALETTE[len(texcolour) % len(PALETTE)])
                    fill, what = f'fill=solid {rgba}', f'solid {name} (contents not captured)'
                    if fmt not in ('10', '11', '70', '71', '80', '81', '115', '125'):
                        fill, what = 'fill=solid 1 1 1 1', f'format {fmt}: harness fills only plain formats; replace this line'
                out.append(f'texture {tm.group(1)} {tm.group(2)} format={fmt} size={w}x{h} {fill}  # texture {tid}: {what}')
        if dm:
            prim, count, ty, off, inst, bv = dm.groups()
            if indices:
                ib = indices[1][:int(count) * (2 if ty == '0' else 4)].hex()
            else:
                ib = re.search(r'indices \d+: (.*)', d['draw']).group(1).replace(' ', '')
            out.append(f'drawindexed {prim} count={count} type={"16" if ty == "0" else "32"} offset={off} instances={inst} basevertex={bv} indices={ib}')
        else:
            am = re.match(r'draw prim (\d+) start (\d+) count (\d+) instances (\d+)', d['draw'])
            if am:
                out.append(f'draw {am.group(1)} start={am.group(2)} count={am.group(3)} instances={am.group(4)}')
            else:
                out.append('# draw: ' + d['draw'])
        vp = re.match(r'(\S+) (\S+) (\d+)x(\d+) z (\S+)-(\S+)', d['viewport'])
        out.append('viewport ' + ' '.join(vp.groups()))
        sc = re.match(r'(\d+) (\d+) (\d+)x(\d+)', d['scissor'])
        out.append('scissor ' + ' '.join(sc.groups()))
        for k in ('raster', 'depthstencil'):
            if k in d:
                out.append(f'# {k}: {d[k]} (not modelled)')
    print('\n'.join(out))


main()
