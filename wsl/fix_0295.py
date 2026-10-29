W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0295: audio step 2, iOS's speaker output to the speakers ------------------------------------------------------
# aud21 (k0294): system sounds reach the "Speaker" device's ring buffer. Boot-arg pandora_spk_live=1 (with
# pandora_spk=3 pandora_amp=3): the speaker graph stays up (no 26 s stop, no test tone), every 20 ms buffer it takes
# comes from pandora_audio_out_fill (the ring, read behind the sample time of now; silence while IO is stopped; the
# ring cleared at each IO start), and the amps keep playing after PLAY (readouts every 10 s for a minute).
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0295 live speaker output)')
