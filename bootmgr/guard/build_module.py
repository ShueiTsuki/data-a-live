"""Pack the guard as a Magisk module: bootmgr/out/pandora_bootmgr_guard.zip.

    python -I bootmgr/guard/build_module.py
Takes guard.sh, service.sh, customize.sh, module.prop from this folder and pinfile from
tools/pinfile (static arm64). The META-INF installer is Magisk's standard module template.
"""
import hashlib
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
OUT = os.path.join(HERE, '..', 'out', 'pandora_bootmgr_guard.zip')

UPDATE_BINARY = '''#!/sbin/sh
umask 022
ui_print() { echo "$1"; }
OUTFD=$2
ZIPFILE=$3
mount /data 2>/dev/null
if [ ! -f /data/adb/magisk/util_functions.sh ]; then
  ui_print "Magisk v20.4 or newer is needed"
  exit 1
fi
. /data/adb/magisk/util_functions.sh
install_module
exit 0
'''


def text(name):
    return open(os.path.join(HERE, name), encoding='utf-8').read().replace('\r\n', '\n').encode()


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    pinfile = open(os.path.join(ROOT, 'tools', 'pinfile', 'pinfile'), 'rb').read()
    assert pinfile[:4] == b'\x7fELF' and int.from_bytes(pinfile[18:20], 'little') == 183, 'pinfile: not arm64'
    with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('META-INF/com/google/android/update-binary', UPDATE_BINARY)
        z.writestr('META-INF/com/google/android/updater-script', '#MAGISK\n')
        for n in ('module.prop', 'service.sh', 'customize.sh', 'guard.sh'):
            z.writestr(n, text(n))
        z.writestr('pinfile', pinfile)
    print(OUT, hashlib.sha256(open(OUT, 'rb').read()).hexdigest())
    print('pinfile', hashlib.sha256(pinfile).hexdigest())


if __name__ == '__main__':
    main()
