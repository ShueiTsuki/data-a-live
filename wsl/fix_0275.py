W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0275: louder, longer test beeps; the amps' own playback readouts ------------------------------------------------
# spk16 (k0273): the tone reached the speaker graph (1005 buffers sent, 1002 back without error) while both amps played
# (AUDIO_STATE 2), but the user did not hear it: 2 s at -30 dBFS during boot. pandora_adsp.c: -20 dBFS beeps,
# 300 ms on and 200 ms off. pandora_amp.c: 8 s of play, and at 1, 4 and 7 s the firmware's readouts that Android's
# mixer dump shows during playback (AUDIO_EN_STATUS, CLOCK_OK, SAMPLING_RATE, ERROR, ABNORMAL_MUTE, AUDIO_IN_MAX,
# VOLTAGE_MAX, CURRENT_MAX), so the log tells whether audio comes in and whether the amp drives the speaker.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0275 beeps, playback readouts)')
