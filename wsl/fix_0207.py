W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# Performance-controller logging (freq5): thread entries freed after a minute without time, names
# taken again at every switch (exec), 4096 threads and 256 process names.
copy('pandora_perfctl.c', 'osfmk/arm/pandora_perfctl.c')
print('patched')
