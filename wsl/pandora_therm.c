/*
 * PANDORA board thermistors, read only (boot-arg pandora_therm=1): the sensors Android's mi_thermald
 * weighs into the "body" temperature it throttles on (thermal-normal.conf, D:\DeepSeekHarness\
 * thermalcheck). They are thermal-monitor channels of PMK8850's vadc@9000 (qcom,spmi-adc5-gen4): the
 * PMIC measures them by itself and leaves each result as a u16 in that channel's SDAM slot, at 0x5c +
 * 2 * slot (Linux qcom-spmi-adc5-gen3 ADC5_GEN3_CH0_DATA0). Slots go in the DT order of the
 * qcom,adc-tm-type channels, SDAM0 (PPID 0x090) slots 1-7 (slot 0 is for one-off conversions), then
 * SDAM1 (PPID 0x091) slots 0-6. A code becomes a temperature as Android reports it (pandora_ntc.h).
 * Against Android's zones of the same moment (thermadc raw sdam_t1/t2, zones_t1/t2) all 14 come out
 * within 0.45 degree, most within 0.05 (tools/hosttest/therm_host.c).
 * Every second the slots are read; every 10 s a line with mi_thermald's seven inputs and its
 * VIRTUAL-SENSOR-FORMULA (five weighted sums, the largest: thermadc §5.1), and how many slots changed,
 * which says whether the PMIC still measures under iOS. "battery" is Android's battery thermal zone;
 * the gauge's temperature stands in for it here. Nothing is limited yet.
 * Under iOS the slots read 0 (bat4): the PMIC measures a channel only once a driver has set it up, and
 * Android's setup does not survive the restart. pandora_therm=2 sets the 14 channels up first, as Linux
 * qcom-spmi-adc-tm5-gen3 adc_tm5_gen3_configure: per channel, on its SDAM, wait until HS (0x45) says
 * ready and CONV_REQ (0xe5) is clear, read the 12 request registers 0x4f-0x5a, write SID (the DT reg's
 * upper byte; gen4 keeps the bus there, as the bootloader's 0x27 for pmih010x), PERPH_CH 0x80 | slot,
 * TIMER_SEL 4 (every second), DIG_PARAM calibration 1 (ratiometric, all 14 have qcom,ratiometric) and
 * decimation 1, FAST_AVG enabled with 1 sample, the ADC channel (the DT reg's lower byte), DELAY_CTL 2
 * (qcom,hw-settle-time 200 us in the gen3 table), CH_EN 0 (no threshold interrupts: nothing here takes
 * them), then CONV_REQ 1. Only these request registers are written; nothing about voltages or power.
 * Decimation 1 is what Android writes (its regmap of vadc@9000: DIG_PARAM 0x14 on both SDAMs, thermadc
 * regmap_0-00_9000_t1); upstream Linux's default, 2, gave SDAM0 slots 1-6 readings 20-26 degrees above
 * Android's while the other eight read like Android (usb5). Within one boot (usb6, k0254) the two
 * decimations gave readings under a degree apart: decimation is not the cause.
 * Those six channels, and cpu_therm, reach the ADC through PMIC GPIOs that Android's pinctrl puts in
 * high impedance (bias-high-impedance: the pin's EN_CTL 0x46 bit 7 cleared, Linux pinctrl-spmi-gpio
 * pmic_gpio_config_set): vadc@9000's own pinctrl-0 (xoagg, pa2, qtm) and the default states of the GPIO
 * controllers of pmh0101 (wireless, cpu) and pmih010x (display, charger). Nobody sets them under iOS.
 * As found under iOS six of them have EN_CTL 0x80 (on), cpu_therm's 0x00; with xoagg's cleared its reading
 * went from 71.9 to 48.2 degrees, next to xo's 47.7, in the same boot (usb7). pandora_therm=2 logs the
 * seven GPIOs' registers (subtype 0x05, 0x40-0x47), clears their EN_CTLs (nothing else of the GPIOs is
 * written), then sets the channels up. pandora_therm_gpio=1 instead sets the channels up first, logs
 * every channel's code after 5 s, clears the EN_CTLs, and logs the codes again 3 s later.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include "pandora_battery.h"
#include "pandora_ntc.h"

extern kern_return_t thread_terminate(thread_t);
extern int pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n);
extern int pandora_spmi_write(uint16_t ppid, uint8_t reg, const uint8_t *buf, unsigned n);
void pandora_therm_init(void);

#define PT_SDAM0     0x090u      /* PMK8850 (SID 0) vadc@9000 */
#define PT_SDAM1     0x091u      /* vadc@9000's second SDAM (DT reg <0x9000 0x9100>, #size-cells 0) */
#define PT_HS        0x45u       /* 0x01 ready */
#define PT_TM_EN_STS 0x47u
#define PT_REQ       0x4fu       /* 0x4f-0x5a: SID PERPH_CH TIMER_SEL DIG_PARAM FAST_AVG CH_SEL DELAY_CTL CH_EN thresholds */
#define PT_DATA      0x5cu
#define PT_CONV_REQ  0xe5u
#define PT_PERIOD_MS 1000u
#define PT_LOG_EVERY 10u

/* reg: the DT reg, SID << 8 | ADC channel */
static const struct {
	const char *name;
	uint8_t sdam, slot;
	uint16_t reg;
} pt_chan[] = {
	{"xoagg", 0, 1, 0x004b}, {"qtm", 0, 2, 0x014b}, {"pa2", 0, 3, 0x0149}, {"display", 0, 4, 0x275e},
	{"charger", 0, 5, 0x274d}, {"wireless", 0, 6, 0x014c}, {"cpu", 0, 7, 0x014d}, {"xo", 1, 0, 0x0044},
	{"wifi", 1, 1, 0x0146}, {"pa1", 1, 2, 0x0147}, {"quiet", 1, 3, 0x0144}, {"flash", 1, 4, 0x0145},
	{"batt", 1, 5, 0x2744}, {"conn", 1, 6, 0x2747},
};
#define PT_CHANS (sizeof(pt_chan) / sizeof(pt_chan[0]))

/* The thermistors' PMIC GPIOs (gpio N at peripheral 0x88 + N - 1, pmk8850's at 0xb8 + N - 1: pinctrl@b800),
 * PPID as pandora_spmi_read takes it: the SID byte (pmih010x 0x27, bus 1) << 8 | peripheral. Under Android
 * all seven read MODE_CTL 00, DIG_PULL 04, DIG_OUT 01, EN_CTL 00 (its regmaps 0-00, 0-01, 1-07, after usb6). */
static const struct {
	const char *name;
	uint16_t ppid;
} pt_gpio[] = {
	{"xoagg pmk8850 gpio7", 0x00be}, {"qtm pmh0101 gpio2", 0x0189}, {"pa2 pmh0101 gpio3", 0x018a},
	{"wireless pmh0101 gpio9", 0x0190}, {"cpu pmh0101 gpio12", 0x0193}, {"display pmih010x gpio18", 0x2799},
	{"charger pmih010x gpio12", 0x2793},
};
#define PT_GPIOS   (sizeof(pt_gpio) / sizeof(pt_gpio[0]))
#define PT_GPIO_EN 0x46u

/* mi_thermald's sensors, in its order: pa_therm1 charger_therm pa_therm2 quiet_therm wifi_therm battery
 * cpu_therm (-1: battery, from the gauge) */
static const int pt_in[7] = {9, 4, 2, 10, 8, -1, 6};

/* thermal-normal.conf VIRTUAL-SENSOR-FORMULA-*: weights over weight_sum 1000, plus compensation */
static const struct {
	const char *name;
	int16_t w[7];
	int32_t comp;
} pt_vs[] = {
	{"front", {-161, -621, -406, 343, 282, 915, 519}, 3314},
	{"back", {-95, -679, 124, -103, 502, 999, 196}, 398},
	{"left", {-711, -501, 983, -326, 147, 777, 553}, 1442},
	{"right", {-724, -833, 965, -534, 760, 947, 348}, 331},
	{"top", {-711, -501, 983, -326, 147, 777, 553}, 1442},
};
#define PT_VS (sizeof(pt_vs) / sizeof(pt_vs[0]))

static int pt_chan_wait;

static void
pt_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pt_chan_wait, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* The eight result slots of one SDAM; false when a read fails. */
static bool
pt_read_slots(uint16_t ppid, uint16_t code[8])
{
	uint8_t b[16];
	if (pandora_spmi_read(ppid, PT_DATA, b, 8) || pandora_spmi_read(ppid, PT_DATA + 8, b + 8, 8)) {
		return false;
	}
	for (unsigned i = 0; i < 8; i++) {
		code[i] = (uint16_t)(b[2 * i] | b[2 * i + 1] << 8);
	}
	return true;
}

/* m as d.dd (millidegrees, two decimals) into s */
static const char *
pt_fmt(char *s, size_t n, int32_t m)
{
	const int32_t a = m < 0 ? -m : m;
	snprintf(s, n, "%s%d.%02d", m < 0 ? "-" : "", a / 1000, (a % 1000) / 10);
	return s;
}

static void
pt_dump_config(uint16_t ppid)
{
	uint8_t b[24] = {0};
	int e = pandora_spmi_read(ppid, 0x45, b, 8);
	e = e ? e : pandora_spmi_read(ppid, 0x4d, b + 8, 8);
	e = e ? e : pandora_spmi_read(ppid, 0x55, b + 16, 8);
	printf("PANDORA THERM: SDAM %#x 0x45-0x5c err %d: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x "
	    "%02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", ppid, e, b[0], b[1], b[2], b[3], b[4], b[5],
	    b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19], b[20], b[21],
	    b[22], b[23]);
}

/* Until SDAM ppid takes a request: HS ready and CONV_REQ clear (Linux adc5_gen3_poll_wait_hs, 150 x 100 us). */
static int
pt_wait_ready(uint16_t ppid)
{
	for (unsigned i = 0; i < 150; i++) {
		uint8_t hs, req;
		if (pandora_spmi_read(ppid, PT_HS, &hs, 1) || pandora_spmi_read(ppid, PT_CONV_REQ, &req, 1)) {
			return -10;
		}
		if (hs == 0x01 && req == 0) {
			return 0;
		}
		delay(100);
	}
	return -11;
}

/* Channel i measured by the PMIC every second from now on, with decimation dec; 0 or the negative step
 * that failed. */
static int
pt_tm_start(unsigned i, unsigned dec)
{
	const uint16_t ppid = pt_chan[i].sdam ? PT_SDAM1 : PT_SDAM0;
	uint8_t b[12];
	int e = pt_wait_ready(ppid);
	e = e ? e : pandora_spmi_read(ppid, PT_REQ, b, 8);
	e = e ? e : pandora_spmi_read(ppid, PT_REQ + 8, b + 8, 4);
	if (e) {
		return e;
	}
	b[0] = (uint8_t)(pt_chan[i].reg >> 8);
	b[1] = 0x80u | pt_chan[i].slot;
	b[2] = 4;
	b[3] = (uint8_t)((b[3] & ~0x3cu) | 1u << 4 | dec << 2);
	b[4] = 0x80;
	b[5] = (uint8_t)pt_chan[i].reg;
	b[6] = 2;
	b[7] = 0;
	const uint8_t req = 0x01;
	e = pandora_spmi_write(ppid, PT_REQ, b, 8);
	e = e ? e : pandora_spmi_write(ppid, PT_REQ + 8, b + 8, 4);
	e = e ? e : pandora_spmi_write(ppid, PT_CONV_REQ, &req, 1);
	return e;
}

static void
pt_tm_setup(unsigned dec)
{
	int err[PT_CHANS];
	unsigned ok = 0;
	for (unsigned i = 0; i < PT_CHANS; i++) {
		err[i] = pt_tm_start(i, dec);
		ok += err[i] == 0;
	}
	uint8_t en0 = 0, en1 = 0;
	(void)pandora_spmi_read(PT_SDAM0, PT_TM_EN_STS, &en0, 1);
	(void)pandora_spmi_read(PT_SDAM1, PT_TM_EN_STS, &en1, 1);
	printf("PANDORA THERM: thermal monitor set up (decimation %u), %u of %u channels (errors %d %d %d %d %d %d %d %d "
	    "%d %d %d %d %d %d); TM_EN_STS SDAM0 %02x SDAM1 %02x\n", dec, ok, (unsigned)PT_CHANS, err[0], err[1], err[2],
	    err[3], err[4], err[5], err[6], err[7], err[8], err[9], err[10], err[11], err[12], err[13], en0, en1);
}

/* pandora_therm_gpio=1: each channel's code and temperature, one line, tagged when */
static void
pt_log_codes(const char *when)
{
	uint16_t s0[8], s1[8];
	if (!pt_read_slots(PT_SDAM0, s0) || !pt_read_slots(PT_SDAM1, s1)) {
		printf("PANDORA THERM: %s: SDAM read failed\n", when);
		return;
	}
	char line[640];
	size_t at = 0;
	for (unsigned i = 0; i < PT_CHANS && at < sizeof(line); i++) {
		const uint16_t code = pt_chan[i].sdam ? s1[pt_chan[i].slot] : s0[pt_chan[i].slot];
		const int32_t m = pandora_ntc_mdeg(code);
		const int32_t a = m < 0 ? -m : m;
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %u %s%d.%02d", pt_chan[i].name, code,
		    m < 0 ? "-" : "", a / 1000, (a % 1000) / 10);
	}
	printf("PANDORA THERM: %s:%s\n", when, line);
}

/* Each thermistor GPIO's subtype and registers 0x40-0x47 (MODE_CTL DIG_VIN DIG_PULL DIG_IN DIG_OUT_SRC
 * DIG_OUT EN_CTL -), tagged when. */
static void
pt_gpio_dump(const char *when)
{
	for (unsigned i = 0; i < PT_GPIOS; i++) {
		uint8_t sub = 0, b[8] = {0};
		int e = pandora_spmi_read(pt_gpio[i].ppid, 0x05, &sub, 1);
		e = e ? e : pandora_spmi_read(pt_gpio[i].ppid, 0x40, b, 8);
		printf("PANDORA THERM: GPIO %s %s: err %d subtype %02x 0x40-0x47 %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    pt_gpio[i].name, when, e, sub, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}
}

/* EN_CTL cleared on each thermistor GPIO, as Linux does for bias-high-impedance */
static void
pt_gpio_high_z(void)
{
	int err[PT_GPIOS];
	for (unsigned i = 0; i < PT_GPIOS; i++) {
		const uint8_t off = 0;
		err[i] = pandora_spmi_write(pt_gpio[i].ppid, PT_GPIO_EN, &off, 1);
	}
	printf("PANDORA THERM: thermistor GPIOs to high impedance: errors %d %d %d %d %d %d %d\n", err[0], err[1], err[2],
	    err[3], err[4], err[5], err[6]);
}

static int pt_level, pt_gpio_level;

static void
pt_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	pt_sleep(10000);
	pt_dump_config(PT_SDAM0);
	pt_dump_config(PT_SDAM1);
	if (pt_level >= 2) {
		pt_gpio_dump("as found");
		if (pt_gpio_level) {
			/* one boot, the same temperatures: the codes before and after */
			pt_tm_setup(1);
			pt_sleep(5000);
			pt_log_codes("GPIOs as found");
			pt_gpio_high_z();
			pt_gpio_dump("after");
			pt_sleep(3000);
			pt_log_codes("GPIOs high impedance");
		} else {
			/* Android's order: the pins (pinctrl at probe), then the channels */
			pt_gpio_high_z();
			pt_tm_setup(1);
		}
		pt_sleep(2000);
		pt_dump_config(PT_SDAM0);
		pt_dump_config(PT_SDAM1);
	}
	uint16_t last[PT_CHANS] = {0};
	unsigned changed = 0, reads = 0, errors = 0;
	for (unsigned n = 1;; n++) {
		uint16_t s0[8], s1[8];
		if (!pt_read_slots(PT_SDAM0, s0) || !pt_read_slots(PT_SDAM1, s1)) {
			if (errors++ < 5) {
				printf("PANDORA THERM: SDAM read failed\n");
			}
			pt_sleep(PT_PERIOD_MS);
			continue;
		}
		int32_t t[PT_CHANS];
		for (unsigned i = 0; i < PT_CHANS; i++) {
			const uint16_t code = pt_chan[i].sdam ? s1[pt_chan[i].slot] : s0[pt_chan[i].slot];
			changed += reads && code != last[i];
			last[i] = code;
			t[i] = pandora_ntc_mdeg(code);
		}
		reads++;
		if (n % PT_LOG_EVERY == 0 || n == 1) {
			struct pandora_gauge g;
			const bool gauge = pandora_gauge_get(&g, sizeof(g));
			const int32_t battery = gauge ? (g.temperature_dk - 2730) * 100 : 0;
			int32_t in[7], body = INT32_MIN, vs[PT_VS];
			for (unsigned k = 0; k < 7; k++) {
				in[k] = pt_in[k] < 0 ? battery : t[pt_in[k]];
			}
			for (unsigned v = 0; v < PT_VS; v++) {
				int64_t sum = 0;
				for (unsigned k = 0; k < 7; k++) {
					sum += (int64_t)pt_vs[v].w[k] * in[k];
				}
				vs[v] = pt_vs[v].comp + (int32_t)(sum / 1000);
				body = vs[v] > body ? vs[v] : body;
			}
			char a[7][12], f[PT_VS][12], b[12], o[8][12];
			printf("PANDORA THERM: body %s (front %s back %s left %s right %s top %s) from pa1 %s charger %s pa2 %s "
			    "quiet %s wifi %s battery%s %s cpu %s; slots changed %u in %u reads\n", pt_fmt(b, sizeof(b), body),
			    pt_fmt(f[0], 12, vs[0]), pt_fmt(f[1], 12, vs[1]), pt_fmt(f[2], 12, vs[2]), pt_fmt(f[3], 12, vs[3]),
			    pt_fmt(f[4], 12, vs[4]), pt_fmt(a[0], 12, in[0]), pt_fmt(a[1], 12, in[1]), pt_fmt(a[2], 12, in[2]),
			    pt_fmt(a[3], 12, in[3]), pt_fmt(a[4], 12, in[4]), gauge ? "(gauge)" : "(none)", pt_fmt(a[5], 12, in[5]),
			    pt_fmt(a[6], 12, in[6]), changed, reads);
			printf("PANDORA THERM: others xoagg %s qtm %s display %s wireless %s xo %s flash %s batt %s conn %s\n",
			    pt_fmt(o[0], 12, t[0]), pt_fmt(o[1], 12, t[1]), pt_fmt(o[2], 12, t[3]), pt_fmt(o[3], 12, t[5]),
			    pt_fmt(o[4], 12, t[7]), pt_fmt(o[5], 12, t[11]), pt_fmt(o[6], 12, t[12]), pt_fmt(o[7], 12, t[13]));
			if (n == 1) {
				printf("PANDORA THERM: codes SDAM0 %u %u %u %u %u %u %u %u SDAM1 %u %u %u %u %u %u %u %u\n", s0[0], s0[1],
				    s0[2], s0[3], s0[4], s0[5], s0[6], s0[7], s1[0], s1[1], s1[2], s1[3], s1[4], s1[5], s1[6], s1[7]);
			}
			changed = reads = 0;
		}
		pt_sleep(PT_PERIOD_MS);
	}
}

void
pandora_therm_init(void)
{
	(void)PE_parse_boot_argn("pandora_therm", &pt_level, sizeof(pt_level));
	(void)PE_parse_boot_argn("pandora_therm_gpio", &pt_gpio_level, sizeof(pt_gpio_level));
	if (!pt_level) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(pt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
