W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0268: the amps' firmware read back whole; reloaded after a reset with the clock, two ways of writing -----------
# spk10 (k0267): once started, the Halo core is stopped by the MPU at XM 0xffffe, YM 0x820d6, PM 0x7a90, all outside
# what the firmware loads (XM to 0x1e7e, YM to 0x7aa, PM to 0xf0c); only after the start, never before. A stopped
# core keeps its enable bit, so the reload of 0267 gave up. pandora_amp.c now reads every firmware block back
# (pa_verify_fw, 30 registers per read so no read starts inside a packed group); in the TDM step both amps are reset
# (pa_reset, as cs35l43_probe) and loaded again with the bit clock running, 0x40 in 120-byte transfers, 0x42 one
# transfer per block; the TDM step waits for the boot-time load. pandora_adsp.c: the graph runs 30 s for that.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0268 firmware read back, reload after reset)')
