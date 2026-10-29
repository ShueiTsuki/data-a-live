W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0307: the amps' readouts for 6 s after a speaker start ---------------------------------------------------------
# aud37 (k0306): the speaker still reaches its level about 3 s after a start (the user), the receiver does not. 0305's
# levels against the music file: iOS's output does not rise (it starts a little louder and settles). The firmware's
# readouts every 100 ms for 6 s after a speaker power-up, to tell whether the DSP's output (the amps' input) or the
# amps' own output rises.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0307 speaker start readouts)')
