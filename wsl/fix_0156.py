def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# On PANDORA the kernel text stops being drawn once iOS shows its first frame (the Apple
# logo) unless sweetfb_noconsole=0: the text covered the GUI. It still reaches the log file.
patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    (""" * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).""",
     """ * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 * PANDORA stops it by default; sweetfb_noconsole=0 keeps drawing."""),
    ("""		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {""",
     """#if defined(PANDORA)
		int quiet = 1;
#else
		int quiet = 0;
#endif
		fConsolePaused = true;
		PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet));
		if (quiet != 0) {"""),
])
print('patched')
