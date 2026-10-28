W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0282: the audio device's UID "Speaker", the one VirtualAudio looks the iPhone's speaker up by ---------------
# aud1 (k0281): mediaserverd's HAL activated the device ("activating device 33: PandoraBuiltInSpeaker", 48 kHz
# 2 ch), but nobody opened it: VirtualAudio built no route ("vadInitializationCompleteSemaphore: Timeout", "No Audio
# Device Available", default VAD with 0 output formats, system sound -50). VirtualAudio (/Library/Audio/Plug-Ins/
# HAL/VirtualAudio.plugin) picks its transducer database by MGGetProductType and looks physical devices up by UID:
# "Codec", "Speaker", "Voice", "Reference", "Baseband Voice" (the DT's audio-data nodes cs42l75, cs35l26, ...).
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0282 device UID Speaker)')
