W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0269: the amps loaded and started on a locked PLL, in Android's order; what goes in before the start ---------
# spk11 (k0268): the whole firmware read back without a difference, both ways of writing (one transfer per block,
# 120-byte transfers), at boot and after a reset: the writing is fine. The core still ran off to XM 0xffffe,
# YM 0x820d6, PM 0x7a90. But the reload started the core before the PLL was set: the reset had put 0x2C04 back.
# Android sets the PLL in hw_params (set_sysclk) before DAPM loads and starts the DSP. pandora_amp.c now does the
# reset, configuration, PLL, routing and enables, checks the PLL lock (0x10014 bit 0), then loads and starts:
# 0x40 with firmware, coefficients and scene, 0x42 without the scene (Android lays it on once the DSP runs). At boot
# 0x42 gets the firmware alone, against 0x40 with all of it. pandora_adsp.c: the graph runs 20 s.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0269 load on a locked PLL)')
