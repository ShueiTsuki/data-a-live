W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0237: reset the amplifiers before loading, as cs35l43_probe does -----------------------------------------------
# spk2 (0236) found both amps' DSPs running at our probe (core control 0x81): they keep power across the reboot and
# still run what Android loaded, so the load stopped. With pandora_amp>=2 the reset lines (GPIO 10, 54) now go low for
# 2 ms and high again, then 2 ms before the first transfer.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (amp reset before the load)')
