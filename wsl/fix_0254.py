W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0254: the thermal monitor with Android's decimation ---------------------------------------------------------
# usb5: with upstream Linux's default decimation (2) SDAM0 slots 1-6 read 20-26 degrees above Android's zones while
# the other eight read like Android. Android writes decimation 1 (DIG_PARAM 0x14 on both SDAMs in its regmap of
# vadc@9000). pandora_therm=2 now uses 1; pandora_therm=3 compares 2 and 1 in one boot.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')
print('patched (thermal monitor, decimation 1)')
