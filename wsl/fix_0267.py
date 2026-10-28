W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0267: where the amps' Halo core is stopped; one amp loaded again with the clock running ------------------------
# spk9 (k0266): the ROM stopped right after the reset changed nothing. While the ROM runs, the MPU lock reads 0xbeef
# (as in Android's dump); after the stop the access registers read 3 1 0 0 0, after pa_core_run's writes
# 3 1 ff0000 fffff 0 (so the writes take, at least in the bits these registers have), and once the core starts the
# XM/YM/PM violation status is set and the core stops. pandora_amp.c now also reads the violation addresses and the
# AHB master's error, before and after the start; drops the PLL forcing that never helped; and in the TDM step loads
# 0x40 from the start (stop, firmware, coefficients, scene, MPU, start) with the bit clock running, 0x42 kept as it
# was for comparison. pandora_adsp.c: the graph runs 16 s for that.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0267 violation addresses, reload with clock)')
