W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0230: the real battery, from the SOCCP's battery manager -----------------------------------------------------
# The SOCCP (pid 19) runs from the boot loaders on; Android only attaches to it. pandora_adsp.c now keeps one link per
# remote processor (ADSP and SOCCP: SMEM partition, SMP2P, GLINK, IPCC doorbells) on one thread, attaches to the SOCCP
# (boot-arg pandora_battery=1) and reads its battery manager over PMIC GLINK ("PMIC_RTR_SOCCP_APPS", owner 32778, as
# Linux's qcom_battmgr): a survey of every battery and USB property first, logged, then the battery service's fields
# every 20 s. SweetBattery.cpp shows them on pandora (the PM6150 path is sweet's) and keeps its fixed values while
# there are none.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_battery.h', 'bsd/dev/pandora_battery.h')

B = 'iokit/Kernel/arm/SweetBattery.cpp'
edit(B, '''#include <pexpert/pexpert.h>
''', '''#include <pexpert/pexpert.h>
#if defined(PANDORA)
/* 0230: the SOCCP battery manager's readings (bsd/dev/pandora_adsp.c). */
#include "../../../bsd/dev/pandora_battery.h"
#define SWEET_BATTERY_POLL_MS 20000
#else
#define SWEET_BATTERY_POLL_MS 30000
#endif
''')
edit(B, '''	static unsigned estimatePercent(unsigned mv);
};''', '''	static unsigned estimatePercent(unsigned mv);
#if defined(PANDORA)
	void publishPandora(const struct pandora_battery &b);
#endif
};''')
edit(B, '''void
SweetBattery::poll(void)
{
	uint8_t type, present, usb, path, charger, error, temperature;''', '''#if defined(PANDORA)
/*
 * As UniXNU-sheng's ShengBattery.cpp maps its battery manager's readings: powerd reads
 * CurrentCapacity of MaxCapacity 100 as the percentage, the raw mAh fields beside it, IsCharging,
 * ExternalConnected, voltage and amperage (positive while charging).
 */
void
SweetBattery::publishPandora(const struct pandora_battery &b)
{
	setBatteryInstalled(b.present);
	setExternalConnected(b.external);
	setExternalChargeCapable(b.external);
	setIsCharging(b.charging);
	setAtWarnLevel(false);
	setAtCriticalLevel(b.present && !b.external && b.percent <= 2);
	setCurrentCapacity((unsigned)b.percent);
	setMaxCapacity(100);
	setVoltage((unsigned)b.voltage_mv);
	setAmperage(b.current_ma);
	setCycleCount((unsigned)b.cycles);
	int minutes = b.charging ? b.time_to_full_min : b.time_to_empty_min;
	setTimeRemaining(minutes >= 0 ? minutes : -1);
	putBool("FullyCharged", b.fully_charged);
	putBool("AppleRawExternalConnected", b.external);
	putNumber("InstantAmperage", (UInt32)b.current_ma);
	if (b.temperature_ok) {
		putNumber("Temperature", (UInt32)b.temperature);
	}
	if (b.raw_mah > 0) {
		putNumber("AppleRawCurrentCapacity", (UInt32)b.raw_mah);
	}
	if (b.full_mah > 0) {
		putNumber("AppleRawMaxCapacity", (UInt32)b.full_mah);
		putNumber("NominalChargeCapacity", (UInt32)b.full_mah);
	}
	if (b.design_mah > 0) {
		putNumber("DesignCapacity", (UInt32)b.design_mah);
	}
	putBool("SweetBatteryFallback", false);
	putBool("SweetBatteryTemperatureEstimated", !b.temperature_ok);
	putNumber("SweetBatteryPercent", (UInt32)b.percent);
	OSDictionary *legacy = OSDictionary::withCapacity(7);
	if (legacy) {
		const char *keys[] = {kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey,
		    kIOBatteryVoltageKey, kIOBatteryAmperageKey, kIOBatteryCycleCountKey,
		    kIOBatteryFlagsKey};
		UInt32 values[] = {(UInt32)b.percent, 100, (UInt32)b.voltage_mv, (UInt32)b.current_ma, (UInt32)b.cycles,
		    (UInt32)(kIOBatteryInstalled | (b.external ? kIOBatteryChargerConnect : 0) |
		    (b.charging ? kIOBatteryCharge : 0))};
		for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
			OSNumber *number = OSNumber::withNumber(values[i], 32);
			if (number) {
				legacy->setObject(keys[i], number);
				number->release();
			}
		}
		setLegacyIOBatteryInfo(legacy);
		legacy->release();
	}
	updateStatus();
	if (fLastMode != 2) {
		IOLog("SWEET BATTERY: SOCCP battery manager, %d%% %d mV %d mA external=%d charging=%d\\n",
		    b.percent, b.voltage_mv, b.current_ma, b.external, b.charging);
		fLastMode = 2;
	}
}
#endif

void
SweetBattery::poll(void)
{
#if defined(PANDORA)
	/* The SOCCP's readings once there are any; until then (and without them) the fixed values. */
	struct pandora_battery b;
	if (pandora_battery_get(&b, sizeof(b))) {
		publishPandora(b);
	} else {
		publish(100, 4200, 0, true, true, false, true, false, false);
	}
	return;
#endif
	uint8_t type, present, usb, path, charger, error, temperature;''')
edit(B, '''	fReal = real == 1 && sweet_spmi_init() == 0;''', '''#if defined(PANDORA)
	/* No PM6150 here: the timer always runs and poll() takes the SOCCP's readings. */
	(void)real;
	fReal = true;
#else
	fReal = real == 1 && sweet_spmi_init() == 0;
#endif''')
edit(B, '''		self->poll();
		sender->setTimeoutMS(30000);''', '''		self->poll();
		sender->setTimeoutMS(SWEET_BATTERY_POLL_MS);''')
edit(B, '''			fTimer->setTimeoutMS(30000);''', '''			fTimer->setTimeoutMS(SWEET_BATTERY_POLL_MS);''')
print('patched (battery from the SOCCP)')
