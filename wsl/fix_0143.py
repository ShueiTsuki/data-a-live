def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


open('bsd/dev/pandora_adsp.c', 'w').write(open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_adsp.c').read().replace('\r', ''))
patch('bsd/dev/sweet_ufs.c', [
    ("extern void pandora_selftest_init(void);\n", "extern void pandora_selftest_init(void);\nextern void pandora_adsp_init(void);\n"),
    ("""	pandora_selftest_init();
#endif""", """	pandora_selftest_init();
	/* Audio DSP bring-up, boot-arg pandora_adsp (bsd/dev/pandora_adsp.c). */
	pandora_adsp_init();
#endif"""),
])
patch('bsd/conf/files', [
    ("bsd/dev/pandora_selftest.c\t\tstandard\n", "bsd/dev/pandora_selftest.c\t\tstandard\nbsd/dev/pandora_adsp.c\t\t\tstandard\n"),
])
print('patched')
