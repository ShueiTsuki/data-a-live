W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0288: boot-arg pandora_audio_delay=SECONDS publishes the audio device later --------------------------------
# aud9 (k0287 + ADT /product/audio): VirtualAudio takes the device and makes the speaker port, but routes the default
# device while the ringer state is still "off" (SpringBoard sets it 0.3 s later): "Culling route connection because
# ringer switch is off" (pap -> pspk), the route stays on Null_Device, system sounds (1104 key clicks, 1106) play to
# it, the volume slider does not move. Published after the ringer is set, the speaker should be routed.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0288 pandora_audio_delay)')
