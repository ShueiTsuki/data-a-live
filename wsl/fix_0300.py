W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0300: the receiver; graphs and amps follow the route -----------------------------------------------------------
# aud29 (k0299): VirtualAudio built the receiver port 'prec' and the microphone port 'pmbi' on the "Codec" device.
# Now the receiver's hardware, as Android plays on the earpiece (miccheck\android: probe_earpiece.txt, the handset
# graph's keys): PandoraAudio.cpp picks the route (the receiver while the Codec runs with its output stream active,
# else the speaker while it runs, else none); pandora_adsp.c opens that route's graph (spkgraph.bin or rcvgraph.bin,
# run-time instances from the file's tagged modules) with PRM's TDM clock and closes it when the route goes, as PAL
# opens and closes a stream's graph; pandora_amp.c powers the amps in the route's mode (the receiver: the top amp
# alone, its DSP bypassed, gain 13, GPIO 174 low).
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0300 receiver, route)')
