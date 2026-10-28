W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0266: the amps' ROM stopped right after the reset, as cs35l43_probe does -------------------------------------
# spk8 (k0265): with the PLL set as Android sets it and the core restarted with the clock running, the Halo core
# still did not run. Its MPU had taken none of the configuration (access registers 3 1 ff0000 fffff 0 where
# Android's dump has 0xffffffff, lock 0x800000 where Android reads 0xbeef, violations in the XM/YM/PM status
# registers) and XM read 0. Android stops the ROM a few writes after the reset (ids, patch, interrupt masks,
# cs35l43_dsp_init -> cs_dsp_stop); we stopped it after reading the firmware files, the second amp seconds later.
# pandora_amp.c now stops both right after the reset and logs the MPU before and after the stop and after start.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0266 ROM stopped right after reset)')
