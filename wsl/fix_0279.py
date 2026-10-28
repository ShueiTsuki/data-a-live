W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0279: bottom and top speakers apart: 500 Hz bottom, 1 kHz top, then both -----------------------------------
# spk20 (k0278): the speakers play; the user could not tell whether one or both ("可以上下用不同频率").
# pandora_adsp.c: from the tone's start, 3-8 s bottom only at 500 Hz (right, TDM slot 1), 9-14 s top only at 1 kHz
# (left, slot 0), 15-18 s both; 20 ms fades. pandora_amp.c: 18 s of play, no register dump.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0279 bottom 500 Hz, top 1 kHz)')
