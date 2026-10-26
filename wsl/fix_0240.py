W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0240: GPI DMA I2C, the fuel gauge read -----------------------------------------------------------------------
# spk2: the gauge's SE (wrapper 1 SE5) is GSI-only. pandora_gpi.c drives one GPII polled, with the register map, ring
# contexts, commands and TREs of Linux 6.6 gpi.c and i2c-qcom-geni's GPI mode (encoders host-tested). pandora_gauge=2
# opens a free GPII of qcom,gpii-mask, reads ten bq27z561 registers at 0x55 and closes it again.
copy('pandora_gpi.c', 'bsd/dev/pandora_gpi.c')
copy('pandora_gpi.h', 'bsd/dev/pandora_gpi.h')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
edit('bsd/conf/files', 'bsd/dev/pandora_csfw.c\t\t\tstandard\n',
     'bsd/dev/pandora_csfw.c\t\t\tstandard\nbsd/dev/pandora_gpi.c\t\t\tstandard\n')
print('patched (GPI DMA I2C, gauge read)')
