W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# CPU performance probe, read only: the scheduler's clusters, each cluster's clock, and the CPUCP firmware's
# SCMI performance protocol (domains, levels, limits, current level) over the pdp channel Android's
# scmi-cpufreq uses. Boot-arg pandora_cpu=0 turns it off.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
edit('bsd/conf/files', 'bsd/dev/pandora_gpu.c\t\t\tstandard\n',
     'bsd/dev/pandora_gpu.c\t\t\tstandard\nbsd/dev/pandora_cpu.c\t\t\tstandard\n')
edit('bsd/dev/sweet_ufs.c', 'extern void pandora_gpu_init(void);\n',
     'extern void pandora_gpu_init(void);\nextern void pandora_cpu_init(void);\n')
edit('bsd/dev/sweet_ufs.c', '''	pandora_gpu_init();
#endif''', '''	pandora_gpu_init();
	/* CPU performance probe, boot-arg pandora_cpu (bsd/dev/pandora_cpu.c). */
	pandora_cpu_init();
#endif''')
print('patched')
