W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0301: a graph always runs ----------------------------------------------------------------------------------------
# aud30 (k0300): the boot's speaker graph closed when no route was asked for, and the TDM clock was released; 60 ms later
# the audio DSP crashed: "systemcache.c:1048:F03C0C44: Island SCID 12 is not configured" (idle, it enters its island
# low-power mode, which needs an LLCC slice that Android's kernel configures and ours does not). Every later request
# waited out its timeout and the UI stalled behind mediaserverd. Now the graph stays up when nothing plays (silence,
# the amps down), and a change of route closes one graph and opens the other with the clock held.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0301 a graph always runs)')
