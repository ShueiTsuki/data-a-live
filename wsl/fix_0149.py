def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


patch('osfmk/prng/prng_random.c', [
    ("""	/* Ensure the boot seed exists before registration erases the early DRBG. */
	(void)early_random();
""", """	/*
	 * A prelinked original corecrypto kext (tools/mkprelink.py) installs the kernel PRNG
	 * itself when IOKit loads it, which is before this EARLY_BOOT step: keep Apple's.
	 */
	if (prng_ready) {
		kprintf("SWEET PRNG: kernel PRNG already installed (corecrypto kext); built-in not registered\\n");
		return;
	}
	/* Ensure the boot seed exists before registration erases the early DRBG. */
	(void)early_random();
"""),
])
print('patched')
