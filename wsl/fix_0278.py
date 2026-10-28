W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0278: the amps' 0x0104 / 0x0124 left at the chip's 1; Android's playback start order; full dump at 2 s -------
# spk19 (k0277): both amps had input, V and I for 15 s; nothing heard. Android's music player start traced (ftrace
# regmap, both amps): 0x4C00 0x36, 0x6808 0x3F75 (noise gate), 0x4800 0x10001, 0x2018 0x3721, scene, key + 0x2084,
# 0x2014 1, 0x10110 bit 31, PLAY; ours the same but 0x6808 and the order of 0x2018 bit 0. Android's chips read
# with the regmap cache bypassed: our configuration table matches but for 0x0104 / 0x0124, 1 on the chip (the old
# dump's 0 was the driver's cache default), which we wrote 0. pandora_amp.c: those two left alone, Android's order
# and 0x6808, and both amps' ordinary registers dumped at 2 s of play.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0278 amp 0x0104/0x0124, start order, dump)')
