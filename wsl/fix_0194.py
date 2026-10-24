W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# 128 IBs per submission (NativeAdrenoABI.h edited by tools/turnip/abi_128.py, Turnip built with the
# same header): bb20 hung with BR and BV inside user IBs while Turnip split submissions into kernel
# submissions of 16. The 16-buffer layout is still taken. The hang dump adds IB2/IB3 and the IB words
# around each pipe's fetch point.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
copy('adreno/NativeAdrenoABI.h', 'iokit/Kernel/arm/NativeAdrenoABIv1.h')
print('patched')
