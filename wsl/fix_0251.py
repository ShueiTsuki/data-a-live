import os

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0251: map the USB module windows from kernel_map --------------------------------------------------
# usb3: ml_io_map takes its windows from the 8 MiB io_submap for good; the three USB windows (2.25 MiB) left a
# later 128 KiB driver mapping without room (panic at 5 s). qcom_usb_debug_map uses ml_io_map_unmappable.
for d in ('include', 'kernel', 'soc', 'usb'):
    for name in sorted(os.listdir(W + 'qcomusb/' + d)):
        copy('qcomusb/%s/%s' % (d, name), 'bsd/dev/qcomusb/%s/%s' % (d, name))
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')
print('patched (USB windows from kernel_map)')
