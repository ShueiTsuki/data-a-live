W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0277: a 15 s steady tone at -12 dBFS, readouts every second -----------------------------------------------------
# spk18 (k0276): with the stream volume at unity both amps got the beeps (AUDIO_IN_MAX at 1 s: bottom 0x2c0, top 0x311;
# V/I max well above zero), readouts that fell in the beeps' gaps read near 0; the user heard nothing. pandora_adsp.c:
# one steady 1 kHz tone at -12 dBFS; the graph runs 26 s. pandora_amp.c: 15 s of play with both amps' readouts every
# second.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0277 steady tone, 15 s)')
