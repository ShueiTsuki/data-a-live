W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0245: plugging and unplugging show at once ---------------------------------------------------------------------
# spk6: the SOCCP sends MCA notifications (opcode 0x12) when the USB cable goes out and in, and our pass right after
# reads bus_volt 26-38 mV unplugged, 4.7-5.0 V plugged. A pass that sees the charger change tells SweetBattery at once
# (pandora_battery_changed: its timer fires now) and kicks the gauge thread for a fresh read; the gauge thread also
# tells SweetBattery whenever what it shows changes (percentage, charger, charging, full).
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_battery.h', 'bsd/dev/pandora_battery.h')

S = 'iokit/Kernel/arm/SweetBattery.cpp'
edit(S, '''#if defined(PANDORA)
	void publishPandora(const struct pandora_battery &b);
#endif
};''', '''#if defined(PANDORA)
	void publishPandora(const struct pandora_battery &b);
public:
	void pollNow(void);
#endif
};''')
edit(S, '''OSDefineMetaClassAndStructors(SweetBattery, IOPMPowerSource);
''', '''OSDefineMetaClassAndStructors(SweetBattery, IOPMPowerSource);

#if defined(PANDORA)
/* The running instance, for pandora_battery_changed (bsd/dev: charger plugged, gauge read changed). */
static SweetBattery *gPandoraBattery;

/* The timer fires now instead of at its next 20 s; it rearms itself after the poll. */
void
SweetBattery::pollNow(void)
{
	if (fRunning && fTimer) {
		fTimer->setTimeoutMS(1);
	}
}

extern "C" void
pandora_battery_changed(void)
{
	SweetBattery *battery = gPandoraBattery;
	if (battery) {
		battery->pollNow();
	}
}
#endif
''')
edit(S, '''			fTimerAdded = true;
			fRunning = true;
			fTimer->setTimeoutMS(SWEET_BATTERY_POLL_MS);''', '''			fTimerAdded = true;
			fRunning = true;
			fTimer->setTimeoutMS(SWEET_BATTERY_POLL_MS);
#if defined(PANDORA)
			gPandoraBattery = this;
#endif''')
edit(S, '''	self->fRunning = false;
	self->fTimer->cancelTimeout();''', '''#if defined(PANDORA)
	gPandoraBattery = NULL;
#endif
	self->fRunning = false;
	self->fTimer->cancelTimeout();''')
print('patched (plugging shows at once)')
