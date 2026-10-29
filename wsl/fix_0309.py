W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0309: a screen shot on any two keys held together ------------------------------------------------------------
# shot1 (k0308): the user took shots the way phones do, power + volume up (the iPhone's keys) and power + volume down
# (Android's); only volume up + volume down counted, so none was saved. Any two of the three now.
copy('pandora_files.c', 'bsd/dev/pandora_files.c')
patch('bsd/dev/fuxi_keys.c', [
    ("""		/* 0308: volume up and volume down held together: one screen shot. The keys still go to iOS. */
		const bool chord = state[1] && state[2];
		if (chord && !shot_chord) {
			printf("PANDORA KEYS: volume up + volume down: screen shot\\n");""",
     """		/* 0308: two keys held together: one screen shot (0309: any two; power + volume up is the iPhone's, power +
		 * volume down Android's). The keys still go to iOS. */
		const bool chord = state[0] + state[1] + state[2] >= 2;
		if (chord && !shot_chord) {
			printf("PANDORA KEYS: %s%s%s: screen shot\\n", state[0] ? "power " : "", state[1] ? "volume-up " : "",
			    state[2] ? "volume-down" : "");"""),
])
print('patched (0309 any two keys)')
