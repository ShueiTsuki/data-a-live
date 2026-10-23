W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# Adreno 840 bring-up, level 14: buffer objects, client windows, submissions and fences in
# bsd/dev/pandora_gpu.c (pandora_gpu=14), and the NativeAdreno user-client ABI over them in
# iokit/Kernel/arm/PandoraAdrenoGpu.cpp (NativeAdrenoABI.h copied unchanged, MIT).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
# The fuxi lane's NativeAdrenoABI.h in the same directory is its version 2; Turnip's Darwin
# backend speaks version 1, kept under its own name. LICENSE.NativeAdreno is already there.
copy('adreno/NativeAdrenoABI.h', 'iokit/Kernel/arm/NativeAdrenoABIv1.h')
patch('iokit/conf/files.arm64', [
    ("iokit/Kernel/arm/PandoraPMGRNub.cpp                              optional iokitcpp\n",
     "iokit/Kernel/arm/PandoraPMGRNub.cpp                              optional iokitcpp\n"
     "iokit/Kernel/arm/PandoraAdrenoGpu.cpp                            optional iokitcpp\n"),
])
patch('iokit/KernelConfigTables.cpp', [
    ("""    "     'IOMatchCategory' = AppleKeyStore;"
    "   },"
""", """    "     'IOMatchCategory' = AppleKeyStore;"
    "   },"
    "   {"
    "     'IOClass'         = PandoraAdrenoGpu;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = PandoraAdrenoGpu;"
    "   },"
"""),
])
print('patched')
