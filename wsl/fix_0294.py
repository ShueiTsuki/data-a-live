W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0294: IO also starts with method 0 (engine start) ------------------------------------------------------------
# aud20 (k0293): the default device is routed to our speaker as soon as it appears (Hawking present; the ringer did
# not matter), and the plug-in sound 1106 starts IO on it: method 0 (engine start), no method 10. HAL waits for the
# first zero time stamp, gives up after 5 s ("the IO thread failed to start, Error: 'nope'") and sends method 1. The
# clock now starts with method 0 or 10 and stops with 1 or 11.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0294 engine start runs the clock)')
