def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0289: the ring/silent switch reads "ring" -------------------------------------------------------------------
# aud9-aud11: VirtualAudio culls the speaker from the system sounds' route ("Culling route connection because ringer
# switch is off", pap -> pspk) and SpringBoard sets "SBRingerControl setRingerMuted: true" at boot although its
# preference SBRingerMuted is false: the iPhone X has a ring/silent switch, this phone none. backboardd's
# BKButtonHIDEventProcessor (-initWithContext:) asks the HID system for the current keyboard event of Telephony page
# 0x0B usage 0x2E (copy event, type keyboard) and takes its down field as the ringer state (0 "silent", 1 "tone1");
# with no answer it starts silent. Later 0x0B/0x2E events change it (-processEvent:sender:dispatcher:).
# SweetHIDButtons now answers that query: the switch reads "ring" (boot-arg sweetringer=0: "silent").
P = 'iokit/Kernel/arm/hid/SweetHID.cpp'
patch(P, '''	void key(UInt32 usage, bool down);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }''', '''	void key(UInt32 usage, bool down);
	virtual IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options) APPLE_KEXT_OVERRIDE;
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }''')
patch(P, '''void
SweetHIDButtons::key(UInt32 usage, bool down)
{''', '''/*
 * PANDORA 0289: the ring/silent switch. backboardd's BKButtonHIDEventProcessor asks at start for the current keyboard
 * event of Telephony page 0x0B usage 0x2E and takes its down field as the ringer state (0 "silent", 1 "tone1"). The
 * phone has no switch; without an answer iOS starts silent (SpringBoard mutes the ringer, VirtualAudio keeps the
 * speaker out of system sounds). The switch reads "ring"; boot-arg sweetringer=0 makes it "silent".
 */
IOHIDEvent *
SweetHIDButtons::copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options)
{
	if (type == kIOHIDEventTypeKeyboard && matching != NULL &&
	    matching->getIntegerValue(kIOHIDEventFieldKeyboardUsagePage) == kHIDPage_Telephony &&
	    matching->getIntegerValue(kIOHIDEventFieldKeyboardUsage) == 0x2e) {
		UInt32 ring = 1;
		(void)PE_parse_boot_argn("sweetringer", &ring, sizeof(ring));
		IOLog("SWEET HID: ring/silent switch asked: %s\\n", ring ? "ring" : "silent");
		return IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_Telephony, 0x2e, ring != 0);
	}
	return IOHIDEventService::copyEvent(type, matching, options);
}

void
SweetHIDButtons::key(UInt32 usage, bool down)
{''')
print('patched (0289 ring/silent switch reads ring)')
