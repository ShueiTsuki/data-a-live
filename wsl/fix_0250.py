import os

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0250: map the USB module's windows before it masks interrupts --------------------------------------------------
# usb2: qcom_usb_debug_init mapped its GCC/DWC3/SMMU windows inside its interrupts-masked section, and ml_io_map
# allocates: "Calling {k,z}alloc from interrupt disabled context isn't allowed" at 0.19 s. qcom_usb_debug_map now maps
# them first, from pandora_usb.c's thread with interrupts enabled, and init uses windows already mapped.
for d in ('include', 'kernel', 'soc', 'usb'):
    for name in sorted(os.listdir(W + 'qcomusb/' + d)):
        copy('qcomusb/%s/%s' % (d, name), 'bsd/dev/qcomusb/%s/%s' % (d, name))
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')
print('patched (USB windows mapped first)')
