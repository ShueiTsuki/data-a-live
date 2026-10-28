W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0265: the amps' PLL as Android sets it, the Halo core restarted with the clock -------------------------------
# spk7 (k0262): with the speaker graph running, the TDM clock reached the amps and their power stage ran as in
# Android's playback dump, but the Halo core stayed at core control 0x1 (Android 0x81) with the heartbeat at 0.
# pandora_amp.c's TDM step now sets the PLL the way cs35l43_component_set_sysclk does at each stream start (0x2C04
# open loop, reference frequency, PLL off, source, closed loop, PLL on; FS_MON 0x2D10; GLOBAL_FS 0x2C0C as
# cs35l43_pcm_hw_params) instead of writing 0x430 at once; if the heartbeat still does not count, it stops the core
# (cs_dsp_halo_stop_core) and starts it again while the clock runs. It also reads the Halo registers Android's dump
# has and dumps all registers after power-up, against Android's dump during playback. Shorter log lines.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0265 amp PLL and Halo restart)')
