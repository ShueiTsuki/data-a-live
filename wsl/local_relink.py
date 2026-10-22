#!/usr/bin/env python3
"""Link a PANDORA kernel on Linux from locally compiled objects (cctools-port ld64).

  local_relink.py LOG BUILDTREE OBJDIR OUT [--lib DIR]...

LOG       CI log of an xnu.yml run (for the four link-only objects and the ld command)
BUILDTREE extracted xnu-buildtree artifact, xnu/ = the patched source to build
OBJDIR    objects from `XNU_CC_OBJDIR=OBJDIR xnu-cc.py LOG BUILDTREE` (named after the source)
OUT       kernel.development.pandora.unstripped is written here; strip -S gives the kernel
--lib     directories with libcc_kext.a / libfirehose_kernel.a; without them the link
          reports the missing symbols.
--ci DIR  use the CI xnu-objects artifact (DIR/obj, DIR/linklibs) first; --local SRC (repeatable)
          takes that object from OBJDIR instead.
"""
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, '/opt/work/tools')
xcc = __import__('xnu-cc')

LD = '/opt/work/ld64/inst/bin/aarch64-apple-darwin-ld'
CONFIG = 'build/xnu.obj/DEVELOPMENT_ARM64_PANDORA'


def log_lines(log):
    for line in open(log, errors='replace'):
        line = re.sub(r'^\S+Z ', '', line.rstrip('\n'))
        yield line


def extra_objects(log, bt, objdir):
    """version.o, lastkerneldataconst.o, lastkernelconstructor.o, nonlto.o."""
    want = {'version.o', 'lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o'}
    cmds = {}
    for line in log_lines(log):
        m = re.search(r'(\S+/clang) (-c .*)', line)
        if not m:
            continue
        args = shlex.split(m.group(2))
        srcs = [a for a in args if a.endswith('.c')]
        name = os.path.basename(srcs[-1])[:-2] + '.o' if srcs else None
        if name in want:
            cmds[name] = args
    missing = want - cmds.keys()
    assert not missing, f'no compile command for {missing}'
    cwd = os.path.join(bt, CONFIG)
    out = {}
    for name, args in cmds.items():
        new = [xcc.CLANG, '-target', 'arm64-apple-macos14.0', '-c', '-o', os.path.join(objdir, 'link__' + name),
               '-D__builtin_xnu_type_signature(...)=""', '-D__builtin_xnu_type_summary(...)=0',
               '-D__builtin_xnu_types_compatible(...)=1']
        skip = False
        for a in args:
            if skip:
                skip = False
                continue
            if a in xcc.DROP:
                continue
            if a in xcc.DROP_WITH_ARG:
                skip = True
                continue
            a = xcc.CI_SDK_PLATFORM.sub(os.path.join(bt, 'sdk-kernel-platform/platform'), a)
            new.append(a.replace(xcc.CI_DXB, bt))
        p = subprocess.run(new, cwd=cwd, capture_output=True, text=True)
        assert p.returncode == 0, (name, p.stderr[-2000:])
        out[name] = os.path.join(objdir, 'link__' + name)
    return out


def link_command(log):
    for line in log_lines(log):
        if '/clang++ -nostdlib' in line and 'kernel.development.pandora.unstripped' in line:
            return shlex.split(line[line.index('/Applications'):])
    raise SystemExit('no kernel link command in the log')


def main():
    log, bt, objdir, out = sys.argv[1:5]
    objdir, out = os.path.abspath(objdir), os.path.abspath(out)
    libs = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--lib']
    # --ci DIR: the CI xnu-objects artifact (objects.tgz unpacked into DIR/obj, linklibs.tgz
    # into DIR): its Xcode-built objects are used first, local ones only for what it lacks
    # or for sources named with --local (files changed since that CI run).
    ci = next((os.path.abspath(sys.argv[i + 1]) for i, a in enumerate(sys.argv) if a == '--ci'), None)
    local_only = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--local']
    if ci:
        libs.append(os.path.join(ci, 'linklibs'))
        cc = os.path.join(ci, 'linklibs', 'libcc_kext.a')
        if not os.path.exists(cc):
            os.symlink('libclang_rt.cc_kext.a', cc)
    bt = os.path.abspath(bt)
    # Locally compiled objects by component and object name.
    have = {}
    for f in os.listdir(objdir):
        m = re.match(r'(.*)\.(c|cpp|s|S)\.o$', f)
        if not m:
            continue
        parts = m.group(1).split('__')
        comp = next((c for c in xcc.COMPONENTS if c in parts), None)
        stem = parts[-1]
        have.setdefault((comp, stem), []).append(os.path.join(objdir, f))
        have.setdefault((None, stem), []).append(os.path.join(objdir, f))
    filelist, missing = [], []
    for line in open(os.path.join(bt, CONFIG, 'link.filelist')):
        path = line.strip()
        if not path:
            continue
        comp = path.split('/' + CONFIG.split('/')[-1] + '/')[1].split('/')[0]
        stem = os.path.splitext(os.path.basename(path))[0]
        if ci and not any(stem == os.path.splitext(os.path.basename(l))[0] for l in local_only):
            cip = os.path.join(ci, 'obj', path.split('/build/xnu.obj/')[1])
            if os.path.exists(cip):
                filelist.append(cip)
                continue
        cands = have.get((comp, stem)) or have.get((None, stem), [])
        if len(cands) != 1:
            missing.append((path.split(CONFIG)[1], len(cands)))
            continue
        filelist.append(cands[0])
    print(f'link.filelist: {len(filelist)} objects found, {len(missing)} not: {missing[:8]}')
    if ci:
        n_ci = sum(1 for f in filelist if f.startswith(ci))
        print(f'  {n_ci} from CI, {len(filelist) - n_ci} local')
        extra = {n: os.path.join(ci, 'obj', CONFIG.split('/')[-1], n)
                 for n in ('version.o', 'lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o')}
    else:
        extra = extra_objects(log, bt, objdir)
    os.makedirs(out, exist_ok=True)
    fl = os.path.join(out, 'link.filelist')
    open(fl, 'w').write('\n'.join(filelist) + '\n')
    args = link_command(log)
    ld = [LD, '-arch', 'arm64']
    i = 1
    while i < len(args):
        a = args[i]
        if a.startswith('-Wl,'):
            ld += a[4:].split(',')
        elif a in ('-filelist',):
            ld += ['-filelist', fl]
            i += 1
        elif a == '-o':
            ld += ['-o', os.path.join(out, 'kernel.development.pandora.unstripped')]
            i += 1
        elif a in extra:
            ld.append(extra[a])
        elif a.startswith('-L'):
            pass
        elif a.startswith('-l'):
            if libs:
                ld.append(a)
        elif a in ('-arch',):
            i += 1
        # clang driver options with no ld meaning
        i += 1
    ld = [x.replace(xcc.CI_DXB, bt).replace('"', '') for x in ld]
    for d in libs:
        ld += ['-L', d]
    ld += ['-platform_version', 'macos', '14.0', '14.0']
    print(' '.join(ld)[:400], '...')
    p = subprocess.run(ld, capture_output=True, text=True)
    print(p.stdout[-3000:], p.stderr[-6000:])
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
