W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# CPU governor, second round (freq2: domain 1 ignored plain level sets, the governor went below the bootloader's
# 1.67 GHz, cores 6-7 looked idle): domain 1 tried four ways, a 1.67 GHz floor unless idle for about 0.2 s,
# every CPU's busyness in the log.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
print('patched')
