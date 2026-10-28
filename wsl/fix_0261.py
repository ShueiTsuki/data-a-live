W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0261: thermalmonitord's computed temperatures are 16.16 ---------------------------------------------------
# usb11: with TG0B, TG0V and NAND published thermalmonitord computes and sends its surface temperatures to
# AppleVTempDispatcher; sendVirtualTemp:temperature: sends degrees in 16.16 fixed point (hundredths / 100 * 65536,
# rounded), not hundredths. Converted to hundredths now. (They track mi_thermald's virtual sensors within 0.3 degree:
# TSBE 36.04 / back 36.19, TSRM 38.90 / side 39.16, TSRR 38.93 / top 39.16, TSFL 37.16 / front 37.19 at 163 s.)
copy('PandoraThermalHID.cpp', 'iokit/Kernel/arm/hid/PandoraThermalHID.cpp')
print('patched (iOS temperatures are 16.16)')
