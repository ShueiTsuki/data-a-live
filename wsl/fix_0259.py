W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0259: the temperatures for iOS's thermal monitor --------------------------------------------------------
# User 2026-10-28: connect the temperatures to iOS's own thermal management, with Xiaomi's model behind them.
# thermalmonitord runs on the phone and finds no sensor ("no HID services"). iokit/Kernel/arm/hid/PandoraThermalHID.cpp
# publishes the iPhone X's HID temperature sensors (PrimaryUsagePage 0xff00 / PrimaryUsage 5, Product = the D22
# table's matchName) and an AppleVTempDispatcher that receives the temperatures thermalmonitord computes. The values
# come from pandora_therm.c: the five sensors the D22 surface formulas use are set so the formulas give mi_thermald's
# virtual sensors; the die sensors from the SoC's tsens (new pandora_cpu_tsens). Only with pandora_therm=2 or more.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('PandoraThermalHID.cpp', 'iokit/Kernel/arm/hid/PandoraThermalHID.cpp')
edit('iokit/conf/files.arm64', 'iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp\n',
     'iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp\n'
     'iokit/Kernel/arm/hid/PandoraThermalHID.cpp                       optional iokitcpp\n')
edit('iokit/KernelConfigTables.cpp', '''    "   {"
    "     'IOClass'         = SweetBattery;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = SweetBattery;"
    "   },"
''', '''    "   {"
    "     'IOClass'         = SweetBattery;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = SweetBattery;"
    "   },"
    "   {"
    "     'IOClass'         = PandoraThermalHID;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = PandoraThermalHID;"
    "   },"
    "   {"
    "     'IOClass'         = AppleVTempDispatcher;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AppleVTempDispatcher;"
    "   },"
''')
print('patched (temperatures for iOS)')
