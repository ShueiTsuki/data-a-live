W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0232: the IPCC interrupt for the remote processors' links ----------------------------------------------------
# The links' thread polled IPCC every 1 ms (50 ms after the first 30 s): too slow for audio periods, too often for an
# idle phone. PandoraIpcc.cpp takes the IPCC's line (/arm-io/ipcc, INTID 278 level, tools/mkadt_pandora.py) as
# PandoraAdrenoGpu.cpp takes the GPU's; its filter calls pandora_ipcc_irq, which takes and clears what rang and wakes
# the thread. The thread sleeps until a doorbell (or its request timers); without the interrupt it polls as before.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('PandoraIpcc.cpp', 'iokit/Kernel/arm/PandoraIpcc.cpp')
edit('iokit/conf/files.arm64', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp                            optional iokitcpp\n',
     'iokit/Kernel/arm/PandoraAdrenoGpu.cpp                            optional iokitcpp\n'
     'iokit/Kernel/arm/PandoraIpcc.cpp                                 optional iokitcpp\n')
edit('iokit/KernelConfigTables.cpp', '''    "     'IOClass'         = PandoraAdrenoGpu;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = PandoraAdrenoGpu;"
    "   },"
''', '''    "     'IOClass'         = PandoraAdrenoGpu;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = PandoraAdrenoGpu;"
    "   },"
    "   {"
    "     'IOClass'         = PandoraIpcc;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = PandoraIpcc;"
    "   },"
''')
print('patched (IPCC interrupt)')
