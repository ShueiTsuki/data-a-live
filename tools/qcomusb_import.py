"""qcomusb_import.py MODULE_ROOT OUT_DIR: copy the kernel units of QcomXnuUsbDebug (nabu-usb-cdc's module/, which
adds the bypass-bank DMA route and the prepare stage to xiaoleGun/QcomXnuUsbDebug, both MIT) into OUT_DIR
(wsl/qcomusb), keeping their directories, so the xnu tree can compile them without the module root on -I:
every quoted #include becomes a path relative to the including file. Tests, scripts and docs are left out.
The SoC header defaults to soc/Sm8850.h (ours, not copied) when QUD_SOC_HEADER is not given."""
import os
import re
import shutil
import sys

root, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
DIRS = ('include', 'kernel', 'soc', 'usb')
SKIP = {'soc/Sm8150.h', 'soc/Sm8550.h'}
INC = re.compile(r'^(\s*#\s*include\s+)"([^"]+)"', re.M)


def resolve(src_dir, name):
    """The file a quoted include means: next to the includer, under the module root, or in include/."""
    for base in (src_dir, root, os.path.join(root, 'include')):
        p = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(p):
            return p
    raise SystemExit('unresolved include "%s" in %s' % (name, src_dir))


copied = []
for d in DIRS:
    for name in sorted(os.listdir(os.path.join(root, d))):
        rel = d + '/' + name
        src = os.path.join(root, d, name)
        if rel in SKIP or not os.path.isfile(src):
            continue
        if not name.endswith(('.c', '.h')) and not name.startswith('LICENSE'):
            continue
        text = open(src, encoding='utf-8').read()
        if name.endswith(('.c', '.h')):
            here = os.path.dirname(src)

            def fix(m):
                target = resolve(here, m.group(2))
                return m.group(1) + '"' + os.path.relpath(target, here).replace(os.sep, '/') + '"'

            text = INC.sub(fix, text)
        if rel == 'include/QcomUsbSoc.h':
            text = text.replace('#ifndef QUD_SOC_HEADER\n#error',
                                '#ifndef QUD_SOC_HEADER\n#define QUD_SOC_HEADER "../soc/Sm8850.h"\n#endif\n'
                                '#ifndef QUD_SOC_HEADER\n#error', 1)
        dst = os.path.join(out, d, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        copied.append(rel)
shutil.copyfile(os.path.join(os.path.dirname(root), 'LICENSE'), os.path.join(out, 'LICENSE.nabu-usb-cdc'))
print('\n'.join(copied))
