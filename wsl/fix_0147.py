W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


for f in ('pandora_files.c', 'pandora_adsp.c'):
    open('bsd/dev/' + f, 'w').write(open(W + f).read().replace('\r', ''))
patch('bsd/dev/sweet_ufs.c', [
    ("extern void pandora_adsp_init(void);\n", "extern void pandora_adsp_init(void);\nextern void pandora_files_init(void);\n"),
    ("""	pandora_adsp_init();
#endif""", """	pandora_adsp_init();
	/* Files from the pinned pack into the root file system (bsd/dev/pandora_files.c). */
	pandora_files_init();
#endif"""),
])
patch('bsd/conf/files', [
    ("bsd/dev/pandora_adsp.c\t\t\tstandard\n", "bsd/dev/pandora_adsp.c\t\t\tstandard\nbsd/dev/pandora_files.c\t\t\tstandard\n"),
])
print('patched')
