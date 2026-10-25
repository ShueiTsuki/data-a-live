W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Date and time. On an iPhone the PMU's RTC driver publishes IORTC and answers the kernel's time-of-day
# question (IOKitInitializeTime -> clock_initialize_calendar -> PEGetUTCTimeOfDay). Here the platform
# expert publishes IORTC (0210) and now answers with the PMK8850's RTC, read the way Linux rtc-pm8xxx
# reads a pmk8350-rtc (rtc@6100: data 0x6148-0x614b little endian, control 0x6146 bit 7 = enabled), plus
# an offset: the phone's DT gives the RTC no allow-set-time and no offset cell, so Android keeps wall time
# as RTC + an offset of its own, in encrypted storage. tools/boot_ios.sh measures that offset in Android
# (date - /sys/class/rtc/rtc0/since_epoch) and passes it as boot-arg pandora_rtc_offset. Read only.
S = 'bsd/dev/fuxi_keys.c'
edit(S, '''	printf("PANDORA KEYS: polling every 20 ms (power=%d volume-up=%d volume-down=%d)\\n",
	    fk_pon_apid != FK_NO_APID, pk_tlmm != 0, fk_pon_apid != FK_NO_APID);
	return true;
}
#endif /* PANDORA */''', '''	printf("PANDORA KEYS: polling every 20 ms (power=%d volume-up=%d volume-down=%d)\\n",
	    fk_pon_apid != FK_NO_APID, pk_tlmm != 0, fk_pon_apid != FK_NO_APID);
	return true;
}

/*
 * 0214: the PMK8850 RTC (SID 0, rtc@6100, PPID 0x061) in seconds as the hardware counts them, read as
 * Linux rtc-pm8xxx reads a pmk8350-rtc: control 0x6146 bit 7 set when the RTC runs, data 0x6148-0x614b
 * little endian; when byte 0 has changed by the time the four were read, read them again. Asked for at
 * IOKitInitializeTime, before the keys start, so it maps the arbiter itself. Read only.
 */
int pandora_rtc_read(uint32_t *secs);
int
pandora_rtc_read(uint32_t *secs)
{
	static uint16_t apid = FK_NO_APID;
	fk_core = fk_core ? fk_core : ml_io_map(PK_CORE_PA, 0x1000);
	pk_map = pk_map ? pk_map : ml_io_map(PK_MAP_PA, 0x8000);
	pk_own = pk_own ? pk_own : ml_io_map(PK_OWN_PA, 0x8000);
	fk_obs = fk_obs ? fk_obs : ml_io_map(PK_OBS_PA, 0x40000);
	if (!fk_core || !pk_map || !pk_own || !fk_obs) {
		return -1;
	}
	if (apid == FK_NO_APID) {
		const uint32_t count = fk_rd(fk_core, 4) & 0x1fffu;
		unsigned owner;
		if (fk_rd(fk_core, 0) < 0x80000000u || count == 0 || count > PK_MAX_APIDS) {
			return -2;
		}
		apid = pk_find_apid(0x061, count, &owner);
		if (apid == FK_NO_APID) {
			return -3;
		}
	}
	uint8_t ctrl = 0, b[4], b0;
	if (!fk_read(apid, 0x46, &ctrl)) {
		return -4;
	}
	if (!(ctrl & 0x80u)) {
		return -5;
	}
	for (int tries = 0; tries < 2; tries++) {
		for (int i = 0; i < 4; i++) {
			if (!fk_read(apid, (uint8_t)(0x48 + i), &b[i])) {
				return -4;
			}
		}
		if (!fk_read(apid, 0x48, &b0)) {
			return -4;
		}
		if (b0 == b[0]) {
			break;
		}
	}
	*secs = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	return 0;
}
#endif /* PANDORA */''')

S = 'iokit/Kernel/arm/SweetPlatformExpert.cpp'
edit(S, '''	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;''', '''	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
#if defined(PANDORA)
	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
#endif''')
edit(S, '''const char *
SweetPlatformExpert::deleteList(void)''', '''#if defined(PANDORA)
extern "C" int pandora_rtc_read(uint32_t *secs);   /* bsd/dev/fuxi_keys.c */

/*
 * 0214: what an iPhone's RTC driver answers: the wall clock in seconds. The PMK8850's RTC counter plus
 * boot-arg pandora_rtc_offset (Android's offset, measured by tools/boot_ios.sh). Without an offset the
 * counter alone, but only when it already looks like a date (2020 or later); else 0 (1970) as before.
 */
long
SweetPlatformExpert::getGMTTimeOfDay(void)
{
	uint32_t raw = 0;
	long long offset = 0;
	const int r = pandora_rtc_read(&raw);
	const bool have = PE_parse_boot_argn("pandora_rtc_offset", &offset, sizeof(offset));
	long t = 0;
	if (r == 0 && have) {
		t = (long)raw + (long)offset;
	} else if (r == 0 && raw >= 1577836800u) {
		t = (long)raw;
	}
	IOLog("PANDORA RTC: counter %u (read %d), offset %lld%s -> %ld\\n", raw, r, offset, have ? "" : " (none)", t);
	return t;
}
#endif

const char *
SweetPlatformExpert::deleteList(void)''')
print('patched')
