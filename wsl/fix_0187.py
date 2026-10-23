W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# BoImport (NativeAdreno ABI method 16): a GPU buffer over the caller's own pinned pages, for
# IOSurfaces (bsd/dev/pandora_gpu.c, iokit/Kernel/arm/PandoraAdrenoGpu.cpp).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
print('patched')
