W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


open('bsd/dev/pandora_adsp.c', 'w').write(open(W + 'pandora_adsp.c').read().replace('\r', ''))
fwext = open(W + 'pandora_fwext.c.txt').read().replace('\r', '')
patch('bsd/dev/sweet_ufs.c', [
    ("static void pandora_log_start(void);\n", "static void pandora_log_start(void);\nstatic void pf_load_extents(void);\n"),
    ("			pl_load_extents();\n", "			pl_load_extents();\n			pf_load_extents();\n"),
    ("\n#endif /* ARM64_BOARD_CONFIG_SWEET */\n", "\n" + fwext + "#endif /* ARM64_BOARD_CONFIG_SWEET */\n"),
])
print('patched')
