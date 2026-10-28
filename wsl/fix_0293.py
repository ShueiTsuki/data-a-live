W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0293: the speaker is plugged in again until VirtualAudio routes to it -----------------------------------------
# aud19 (k0292, speaker at 60 s): with the Hawking device present the default device's route goes to the speaker
# ("Attempting to activate with concrete route(s): vdef pap -> pspk", "Route change succeeded"), and that moment is
# visible here: the client sets our stream active ("set stream active" 1 1). The speaker port is culled while the
# ringer is still off (SpringBoard posts it ~11 s after backboardd reads the switch) and a ringer change alone does not
# reroute, so the speaker is published 3 s after the switch read and plugged in again (new instance, old terminated)
# every 5 s until a stream is set active, up to 12 times. User clients hold a reference to their device.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0293 speaker plugged in again until routed)')
