W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# GPU hang dump (the last submissions and the CP state once the ring timestamp stops short of the
# last fence for 5 s; the watch runs for as long as the system does) and fence-wait timeouts counted
# instead of logged (bb19: they filled the log ring while the GPU was stuck).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
print('patched')
