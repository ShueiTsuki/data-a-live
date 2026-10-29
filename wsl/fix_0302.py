W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0302: the Codec device's microphones, described as iPhone X's driver does ------------------------------------
# aud31 (k0301): the receiver button (play-and-record) played on the speaker. VirtualAudio built the route
# { pap -> prec, pmbi -> pap } but every route with the built-in microphone failed to activate: the microphone's DSP
# chain d22_bottom_mic_general has an AUAGCV2 that refused the 2-channel input format ("DSP (agc2) format setup failed
# with error: (-10868)", "Routing is not supported"). The user: "能不能适配硬件". The Codec device now describes its
# channels as iPhone X's AppleEmbeddedAudio does: "maxChannelCount" (MaxInputChannelCount: this phone's two handset
# microphones), the low-power channel masks and "custom property info", HAL's table for custom properties.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
# The user (aud31): the speaker's sound grows louder at its start. The amps' readouts every 100 ms for 3 s after each
# power-up show which stage rises.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0302 Codec channels)')
