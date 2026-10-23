W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# BoImport of a read-only mapping (bb9: KERN_PROTECTION_FAILURE from prepare): pinned for the
# device to read, mapped read-only for the GPU (bsd/dev/pandora_gpu.c, PandoraAdrenoGpu.cpp).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
print('patched')
