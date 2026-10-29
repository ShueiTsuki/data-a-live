W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0303: the Codec microphone stream offers mono and two channels --------------------------------------------------
# aud32 (k0302): the same refusal of the 2-channel format by the microphone chain's AUAGCV2, and HAL never asked for
# another format (method 5, set stream format), as only one was offered. The stream now offers mono (current) and two
# channels, and method 5 switches between them. The amps' 3 s of readouts after each power-up go again (the user:
# the speaker's slow start was probably an illusion; the readouts showed no rise).
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0303 mono and stereo microphone formats)')
