W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0306: the speaker's channels as iPhone X's -----------------------------------------------------------------------
# aud35 (k0305): the receiver plays (user: "正常了"), the speaker still starts low. The start levels: iOS's channel 0
# full from the start, channel 1 8-12 dB lower and rising by about 7 dB over 5 s (iPhone X's speaker chain shades the
# top speaker, its receiver, by orientation). Channel 0 went to TDM slot 0, the top amp, so the bottom speaker played
# the shaded channel. Channel 0 now goes to the bottom amp, channel 1 to the top one.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0306 speaker channels)')
