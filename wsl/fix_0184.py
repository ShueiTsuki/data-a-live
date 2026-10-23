W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Metal discovery: AGXAccelerator, an IOAcceleratorES (the fuxi lane's in-kernel class) with
# MetalPluginName/MetalPluginClassName, sharing the PandoraAdrenoGpu user client; published only
# with boot-arg pandora_gpu_metal=1 (iokit/Kernel/arm/PandoraAdrenoGpu.cpp).
open('iokit/Kernel/arm/PandoraAdrenoGpu.cpp', 'w').write(open(W + 'PandoraAdrenoGpu.cpp').read().replace('\r', ''))
patch('iokit/KernelConfigTables.cpp', [
    ("""    "     'IOMatchCategory' = PandoraAdrenoGpu;"
    "   },"
""", """    "     'IOMatchCategory' = PandoraAdrenoGpu;"
    "   },"
    "   {"
    "     'IOClass'         = AGXAccelerator;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AGXAccelerator;"
    "   },"
"""),
])
print('patched')
