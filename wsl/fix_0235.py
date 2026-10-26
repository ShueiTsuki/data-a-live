W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0235: the amplifiers' DSP firmware written over I2C and read back --------------------------------------------
# pandora_csfw.c turns the CS35L43 firmware and coefficient files into register writes (host-tested against the
# phone's files and Android's register dump). pandora_amp=2 writes the firmware, coefficients and music scene from
# the pinned pack into each amplifier's DSP memory in long I2C messages and reads back the algorithm list, every
# unpacked XM coefficient block and the start of PM; the DSP is not started, nothing is powered up.
copy('pandora_csfw.c', 'bsd/dev/pandora_csfw.c')
copy('pandora_csfw.h', 'bsd/dev/pandora_csfw.h')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
edit('bsd/conf/files', 'bsd/dev/pandora_amp.c\t\t\tstandard\n',
     'bsd/dev/pandora_amp.c\t\t\tstandard\nbsd/dev/pandora_csfw.c\t\t\tstandard\n')
print('patched (amp DSP firmware over I2C, read back)')
