W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Adreno 840 bring-up, level 1: a read-only probe of GCC, GPUCC, the GPU SMMU and the GMU
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=1; off by default).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
patch('bsd/conf/files', [
    ("""bsd/dev/pandora_files.c			standard
""",
     """bsd/dev/pandora_files.c			standard
bsd/dev/pandora_gpu.c			standard
"""),
])
patch('bsd/dev/sweet_ufs.c', [
    ("""extern void pandora_files_init(void);
""",
     """extern void pandora_files_init(void);
extern void pandora_gpu_init(void);
"""),
    ("""	/* Files from the pinned pack into the root file system (bsd/dev/pandora_files.c). */
	pandora_files_init();
""",
     """	/* Files from the pinned pack into the root file system (bsd/dev/pandora_files.c). */
	pandora_files_init();
	/* Adreno 840 bring-up, boot-arg pandora_gpu (bsd/dev/pandora_gpu.c). */
	pandora_gpu_init();
"""),
])
print('patched')
