W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0238: stop the amplifiers DSP ROM before loading, as cs35l43_dsp_init does -----------------------------------
# spk3 (0237): right after the reset the core control still reads 0x81 and the serial port its defaults: the DSP
# runs its ROM out of reset. cs35l43_dsp_init stops it first (cs_dsp_stop: watchdog off, core EN cleared, soft reset).
# The load now does the same instead of giving up on a running core.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (amp ROM stopped before the load)')
