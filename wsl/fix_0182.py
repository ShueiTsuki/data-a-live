W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# For Turnip on the phone: the chip id with the enabled slice count (bsd/dev/pandora_gpu.c,
# iokit/Kernel/arm/PandoraAdrenoGpu.cpp), and the file installer copying files up to 64 MiB a
# chunk at a time (bsd/dev/pandora_files.c).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
copy('pandora_files.c', 'bsd/dev/pandora_files.c')
print('patched')
