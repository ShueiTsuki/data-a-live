import os

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0249: the console as a USB serial port --------------------------------------------------------------------------
# usb1: the bootloader leaves ssusb@a600000 clocked, powered, in device role and halted, and stream 0x40 on an
# untranslated SMMU bank: the state QcomXnuUsbDebug takes over. Its kernel units (nabu-usb-cdc's module with the
# bypass-bank route; tools/qcomusb_import.py, profile soc/Sm8850.h, three changes of ours marked "Data A Live") go
# into bsd/dev/qcomusb; pandora_usb=2 qcom-usb-debug=1 starts it from a thread on the boot CPU that feeds it the
# console ring and polls the controller.
UNITS = ['kernel/QcomXnuUsbDebug.c', 'kernel/QcomUsbScreenshot.c', 'usb/Dwc3.c', 'soc/QcomUsbQualify.c',
         'soc/QcomUsbDmaDomain.c', 'soc/QcomUsbBypass.c', 'soc/QcomUsbPrepare.c']
for d in ('include', 'kernel', 'soc', 'usb'):
    for name in sorted(os.listdir(W + 'qcomusb/' + d)):
        copy('qcomusb/%s/%s' % (d, name), 'bsd/dev/qcomusb/%s/%s' % (d, name))
copy('qcomusb/LICENSE.nabu-usb-cdc', 'bsd/dev/qcomusb/LICENSE.nabu-usb-cdc')
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')
edit('bsd/conf/files', 'bsd/dev/pandora_usb.c\t\t\tstandard\n',
     'bsd/dev/pandora_usb.c\t\t\tstandard\n' + ''.join('bsd/dev/qcomusb/%s\t\tstandard\n' % u for u in UNITS))
print('patched (USB serial console)')
