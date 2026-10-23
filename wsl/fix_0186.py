W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# The user client answers GetStatus (method 8) with zero fault and suspend counts
# (iokit/Kernel/arm/PandoraAdrenoGpu.cpp).
open('iokit/Kernel/arm/PandoraAdrenoGpu.cpp', 'w').write(open(W + 'PandoraAdrenoGpu.cpp').read().replace('\r', ''))
print('patched')
