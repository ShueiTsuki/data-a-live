W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0283: the audio device's transport type by boot-arg (pandora_audio_tt=1: Thunderbolt) ----------------------
# aud2/aud3 (k0282, UID "Speaker", built-in): the HAL activates the device, VirtualAudio builds no route and logs
# nothing even with its categories at debug. sheng's plug-in on iPadOS 17 is routed only as a Thunderbolt device.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0283 transport type boot-arg)')
