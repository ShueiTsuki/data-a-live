W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0305: the levels the DSP gets after each start -----------------------------------------------------------------
# aud33: the user hears the speaker start low and reach its level after a few seconds; iOS's samples in the ring rise
# by about 9 dB over 4 s where the music file's peaks rise by 1.4 dB. Per channel, every 100 ms for 6 s after each IO
# start, the peak and RMS of what pandora_audio_out_fill hands the DSP.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0305 start levels)')
