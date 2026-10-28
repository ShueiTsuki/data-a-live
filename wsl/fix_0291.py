W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0291: the Thunderbolt audio device takes its own UID ----------------------------------------------------------
# aud12-aud14: with VirtualAudio loaded and the ringer on, no routing category ever gets a route to the built-in
# speaker port that VirtualAudio makes from UID "Speaker" (the iPhone X routing database wants more than this one
# device). Boot-arg pandora_audio_tt=1 (0283) now also sets UID "PandoraSpeaker", so VirtualAudio treats the device
# as a Thunderbolt device of its own (sheng's working path on iPadOS 17) instead of the built-in speaker.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0291 Thunderbolt UID)')
