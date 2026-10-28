W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0287: the audio device takes registry properties from its clients (hog mode) --------------------------------
# aud7 (k0286, VirtualAudio moved back): mediaserverd loads VirtualAudio, opens our device, maps the status page and
# IO buffer; VirtualAudio then fails: "HALB_UCObject::SetProperty: got an error from the IORegistry, Error:
# 0xE00002C7", "Unable to take hog mode.", "Could not add PhysicalDevice ID: 33 UID: Speaker". IOService's
# setProperties answers kIOReturnUnsupported; ours keeps every key ("exclusive access owner" first) and logs it.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
print('patched (0287 audio setProperties)')
