W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0239: amp DSP start diagnostics -------------------------------------------------------------------------------
# spk4 (0238): firmware, coefficients and scene all land (every read-back equal), but after the start the core control
# reads 0x1 (Android 0x81), the MPU lock 0x800000 (0xbeef) and the heartbeat does not move: as if unclocked. When the
# heartbeat stands still the PLL is forced on (0x2C04 bit 16, as cs35l43_dsp_preload_ev does when its clock hook is
# set) and it is read again; then the amp's ordinary registers (the addresses in Android's dump) go to the log for a
# comparison with Android's idle state.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (amp DSP start diagnostics)')
