W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# CPU governor over SCMI (boot-arg pandora_cpu=2): checks that a level set moves the clock, then every 16 ms
# picks each domain's level from its busiest CPU's idle time (schedutil's 1.25 x, walt's hispeed jump), capped at
# the firmware's sustained level and limits. The idle time comes from recount through a small osfmk helper.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('pandora_cpu_idle.c', 'osfmk/arm/pandora_cpu_idle.c')
edit('osfmk/conf/files.arm64', 'osfmk/arm/fuxi_adreno_host.c\tstandard\n',
     'osfmk/arm/fuxi_adreno_host.c\tstandard\nosfmk/arm/pandora_cpu_idle.c\tstandard\n')
print('patched')
