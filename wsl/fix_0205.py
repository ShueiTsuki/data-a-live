W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Logging only (freq5): the signals Apple's performance controller would get (context switches with each thread's
# class, CoreAnimation's frames with their deadlines), taken through sched_perfcontrol_register_callbacks, which
# nothing uses on this phone; the SoC temperatures; every SCMI performance level.
copy('pandora_perfctl.c', 'osfmk/arm/pandora_perfctl.c')
edit('osfmk/conf/files.arm64', 'osfmk/arm/pandora_cpu_idle.c\tstandard\n',
     'osfmk/arm/pandora_cpu_idle.c\tstandard\nosfmk/arm/pandora_perfctl.c\tstandard\n')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
print('patched')
