W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0281: the speaker as iOS's built-in audio device, step 1 (no sound) ---------------------------------------
# Our own IOAudio2Device / IOAudio2DeviceUserClient (Apple's IOAudio2Family is not prelinked, the names are free),
# with the registry description and user-client contract CoreAudio's HALS_IOA2* reads on iOS 16 (PLAN 2026-10-28).
# Step 1 plays nothing: it logs every call, runs IO on its own clock and logs the ring's peak, to see mediaserverd
# find, open, map and write the device. Boot-arg pandora_audio=1.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
patch('iokit/conf/files.arm64',
      'iokit/Kernel/arm/PandoraIpcc.cpp                                 optional iokitcpp\n',
      'iokit/Kernel/arm/PandoraIpcc.cpp                                 optional iokitcpp\n'
      'iokit/Kernel/arm/PandoraAudio.cpp                                optional iokitcpp\n')
patch('iokit/KernelConfigTables.cpp',
      '''    "     'IOMatchCategory' = AppleVTempDispatcher;"
    "   },"
#endif''',
      '''    "     'IOMatchCategory' = AppleVTempDispatcher;"
    "   },"
    "   {"
    "     'IOClass'         = IOAudio2Device;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = IOAudio2Device;"
    "   },"
#endif''')
print('patched (0281 IOAudio2Device step 1)')
