W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# After boot the screen never lights by itself (the user always wakes it with the power button), yet the
# log shows "display on", normal brightness and swaps 1-6 before iOS dims and turns the display off 25 s
# later. The frame dump (0159, every pandora_fbdump seconds) is overwritten by later frames, so what those
# first frames held was never seen. Boot-arg pandora_fbdump_swap=N: one dump, at backboardd's swap N,
# never overwritten.
S = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(S, '''	if (dumpSecs > 0 && fConsolePaused && t0 >= dumpNext) {''',
     '''	static int dumpSwap = -1;   /* 0212: pandora_fbdump_swap=N, one dump at backboardd's swap N */
	if (dumpSwap < 0) {
		dumpSwap = 0;
		PE_parse_boot_argn("pandora_fbdump_swap", &dumpSwap, sizeof(dumpSwap));
	}
	if (dumpSwap > 0 ? (fConsolePaused && get32(rec, 0x50) == (uint32_t)dumpSwap)
	    : (dumpSecs > 0 && fConsolePaused && t0 >= dumpNext)) {''')
print('patched')
