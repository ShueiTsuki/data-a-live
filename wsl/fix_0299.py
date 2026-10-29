W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0299: the "Codec" device (receiver, microphones) --------------------------------------------------------------
# Step 4 (user: "先弄听筒和麦克风吧"). On the iPhone X the receiver and the microphones belong to the "Codec" device
# (CS42L75). VirtualAudio's Device_Codec_Aspen (0x23f8d0) makes a receiver port when the device has
# kAudioDevicePropertyMute on output element 3 and an internal microphone port when its input data sources list a
# built-in microphone (imic, imi1-imi4, ...). A third IOAudio2Device (personality PandoraAudioRole = Codec) with an
# output stream, an input stream, a mute control on output element 3 and a data source selector on the input. Its
# output goes nowhere yet and its input is silent: this step looks at the ports and routes VirtualAudio builds.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
patch('iokit/KernelConfigTables.cpp',
      '''    "     'PandoraAudioRole' = Hawking;"
    "   },"''',
      '''    "     'PandoraAudioRole' = Hawking;"
    "   },"
    "   {"
    "     'IOClass'          = IOAudio2Device;"
    "     'IOProviderClass'  = IOResources;"
    "     'IOMatchCategory'  = PandoraAudioCodec;"
    "     'PandoraAudioRole' = Codec;"
    "   },"''')
print('patched (0299 Codec device)')
