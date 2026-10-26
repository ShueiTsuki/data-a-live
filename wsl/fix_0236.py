W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0236: the amplifiers' DSP started as Android does; the gauge bus looked at ----------------------------------
# pandora_amp=3: cs35l43_dlkm.ko's register patch and DSP sense inputs before the load, then memory ready, the Halo
# MPU opened and the core taken out of reset; core control, MPU lock, HALO_STATE and the heartbeat read back
# (control places from the .wmfw's algorithm descriptions, pandora_csfw.c csfw_find_ctl, host-tested). The
# amplifier stays powered down. pandora_gauge=1: wrapper 1 SE5 clocks, pins and state read; nothing sent.
copy('pandora_csfw.c', 'bsd/dev/pandora_csfw.c')
copy('pandora_csfw.h', 'bsd/dev/pandora_csfw.h')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (amp DSP start, gauge bus status)')
