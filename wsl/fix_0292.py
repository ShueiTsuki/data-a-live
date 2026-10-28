W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0292: the "Hawking" input device next to the speaker ----------------------------------------------------------
# aud18 (VirtualAudio at trace level, speaker published after the ringer was on): VirtualAudio builds the route to the
# speaker ("Successfully Built concrete route { pap -> pspk }"), then drops it because the default device needs its
# alternate devices and the "Mandatory" one, Hawking (vhaw), has no connected 'phki' port: "Connection [ phki / pap ]
# is required and cannot be used for a concrete route: source is connected: 0", "Recursion failed after skipping
# optional alternate VAD". A second IOAudio2Device (personality PandoraAudioRole = Hawking) with UID "Hawking" and one
# input stream gives Device_Hawking_Aspen its 'phki' ("Hawking Input"). Log lines now name the device.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
patch('iokit/KernelConfigTables.cpp',
      '''    "     'IOMatchCategory' = IOAudio2Device;"
    "   },"''',
      '''    "     'IOMatchCategory' = IOAudio2Device;"
    "   },"
    "   {"
    "     'IOClass'          = IOAudio2Device;"
    "     'IOProviderClass'  = IOResources;"
    "     'IOMatchCategory'  = PandoraAudioHawking;"
    "     'PandoraAudioRole' = Hawking;"
    "   },"''')
print('patched (0292 Hawking input device)')
