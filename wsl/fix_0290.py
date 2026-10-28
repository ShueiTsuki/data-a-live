W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0290: the audio device is published 3 s after backboardd reads the ring/silent switch -------------------------
# aud12 (k0289, ringer "tone1"): VirtualAudio still routes the system sounds to Null_Device. It routed them when the
# speaker appeared, while the ringer was still off ("Culling route connection because ringer switch is off"), and on
# the ringer change it only checks the current route ("Ringer switch update." -> "Route did not change"); a full route
# search runs only when ports come or go. backboardd reads the switch once at start (SweetHIDButtons::copyEvent) and
# SpringBoard posts the state some 0.2 s later: the device is published 3 s after that read, by default. Boot-args:
# pandora_audio_delay=SECONDS keeps the 0288 fixed time, pandora_audio_now=1 publishes at once.
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
P = 'iokit/Kernel/arm/hid/SweetHID.cpp'
patch(P, '''IOHIDEvent *
SweetHIDButtons::copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options)
{''', '''extern "C" void pandora_audio_ringer_known(void);

IOHIDEvent *
SweetHIDButtons::copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options)
{''')
patch(P, '''		IOLog("SWEET HID: ring/silent switch asked: %s\\n", ring ? "ring" : "silent");
''', '''		IOLog("SWEET HID: ring/silent switch asked: %s\\n", ring ? "ring" : "silent");
		/* PANDORA 0290: the audio device is published 3 s after this read (PandoraAudio.cpp). */
		pandora_audio_ringer_known();
''')
print('patched (0290 audio device after the ringer read)')
