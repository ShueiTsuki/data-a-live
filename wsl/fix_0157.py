def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# 0156 muted the console at backboardd's first swap, which can carry no layer (nothing on
# screen yet). Mute at the first swap that shows a surface: on the phone that frame is the
# lock screen (amfi2: swaps 1-2 empty, swap 3 = first image after SpringBoard's keybag check).
patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    (""" * PANDORA stops it by default; sweetfb_noconsole=0 keeps drawing.""",
     """ * PANDORA stops it by default from the first swap that shows a surface
 * (empty swaps come first); sweetfb_noconsole=0 keeps drawing."""),
    ("""	bool gpuFrame = false, cpuFrame = false;

	if (!fConsolePaused) {""",
     """	bool gpuFrame = false, cpuFrame = false;
	bool shows = false;

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		if ((mask & (1u << layer)) && get32(rec, 0x54 + 4 * layer) != 0) {
			shows = true;
		}
	}
#if !defined(PANDORA)
	shows = true;
#endif
	if (!fConsolePaused && shows) {"""),
])
print('patched')
