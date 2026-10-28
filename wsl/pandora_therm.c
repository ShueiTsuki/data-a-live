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
 * the gauge's temperature stands in for it here. pandora_therm=2 also limits by it every second, with
 * mi_thermald's tables for the CPU clusters and the GPU (pt_rule below), as long as a whole reading is
 * there; the junction-temperature rules of pandora_cpu.c (105 C and up) stay as the last guard.
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

/* mi_thermald's thermal-normal.conf (thermalcheck raw/data/decrypt.txt [SS-CPU0], [SS-CPU6], [MONITOR-GPU]),
 * all on VIRTUAL-SENSOR-FORMULA (the body temperature), polling 1000: step i holds from trig[i] on until the
 * body is under clr[i]; under the first step nothing is limited. SS-CPU0 and SS-CPU6 cap cpufreq-cpu0 and
 * cpufreq-cpu6 (kHz); MONITOR-GPU sets a level of the kgsl cooling device, which counts qcom,gpu-pwrlevels-1
 * from the top (0 = 1200 MHz .. 17 = 160 MHz): pandora_gpu.c's level count - L, its levels going 1 = 160 MHz
 * .. 18 = 1200 MHz. Every frequency named is in this phone's tables (pandora_cpu.c, pandora_gpu.c). */
#define PT_STEPS 15
static const struct {
	const char *name;
	unsigned n;
	int32_t trig[PT_STEPS], clr[PT_STEPS];
	uint32_t target[PT_STEPS];
} pt_rule[3] = {
	{"cpu0", 15, {25000, 35000, 37000, 39000, 40000, 41000, 42000, 43000, 44000, 45000, 46000, 47000, 48000, 49000, 50000},
	 {23000, 32000, 35000, 37000, 39000, 40000, 41000, 42000, 43000, 44000, 45000, 46000, 47000, 48000, 49000},
	 {2745600, 2745600, 2361600, 2227200, 2227200, 1670400, 1670400, 1440000, 1440000, 1324800, 1324800, 1113600, 1113600,
	  1113600, 1113600}},
	{"cpu6", 15, {25000, 34000, 37000, 39000, 40000, 41000, 42000, 43000, 44000, 45000, 46000, 47000, 48000, 49000, 50000},
	 {23000, 32000, 34000, 37000, 39000, 40000, 41000, 42000, 43000, 44000, 45000, 46000, 47000, 48000, 49000},
	 {2668800, 2438400, 1862400, 1497600, 1267200, 1267200, 1267200, 1132800, 1132800, 1267200, 1267200, 1017600, 1017600,
	  1017600, 1017600}},
	{"gpu", 6, {35000, 37000, 40000, 45000, 47000, 50000}, {33000, 35000, 38000, 43000, 45000, 48000}, {5, 6, 8, 11, 13, 15}},
};
static int pt_step[3] = {-1, -1, -1};

extern void pandora_cpu_body_cap(uint32_t khz0, uint32_t khz6);   /* bsd/dev/pandora_cpu.c */
extern uint32_t pandora_gpu_body_top;                             /* bsd/dev/pandora_gpu.c */
extern const uint32_t pandora_gpu_level_count;

static uint32_t
pt_target(unsigned r)
{
	return pt_step[r] < 0 ? 0 : pt_rule[r].target[pt_step[r]];
}

/* The steps for body temperature b (millidegrees), handed on when one moved; true then. */
static bool
pt_limit(int32_t b)
{
	bool moved = false;
	for (unsigned r = 0; r < 3; r++) {
		int s = pt_step[r];
		while (s + 1 < (int)pt_rule[r].n && b >= pt_rule[r].trig[s + 1]) {
			s++;
		}
		while (s >= 0 && b < pt_rule[r].clr[s]) {
			s--;
		}
		moved |= s != pt_step[r];
		pt_step[r] = s;
	}
	if (moved) {
		pandora_cpu_body_cap(pt_target(0), pt_target(1));
		__atomic_store_n(&pandora_gpu_body_top, pt_step[2] < 0 ? 0 : pandora_gpu_level_count - pt_target(2),
		    __ATOMIC_RELAXED);
	}
	return moved;
}

/* ---- 0259: the temperatures for iOS's thermal monitor ----
 * iokit/Kernel/arm/hid/PandoraThermalHID.cpp publishes one HID temperature sensor per pt_hid entry and asks for
 * its value here; thermalmonitord pairs each by Product with the matchName of a sensor of the iPhone X's table
 * (/System/Library/ThermalMonitor/D22AP-Info.plist, Sensors).
 * Xiaomi's thermal model is the one that counts. On the iPhone X thermalmonitord turns PMU tdev2-4, mic1 and mic2
 * into surface temperatures with fixed formulas of its D22 product class (updateAllThermalLoad), each one of them
 * plus the battery, and its hotspots watch those surfaces. Those five are therefore given the values that make its
 * formulas come out at mi_thermald's virtual sensors: TSBE (back, main board: the MLB hotspots) = back, TSRM (frame,
 * main board) = the higher of left and right, TSRR (frame, radio) = top, TSFC (rear camera) = top, TSFL (display)
 * = front. In hundredths of a degree, b the battery (IOPMPowerSource "Temperature", SweetBattery's gauge reading,
 * the same as here): TSBE = 222.2 + 0.45 b + 0.48 tdev2 (+ 0.025 x the rear camera die: no camera here), TSRM =
 * -80.17 + 0.5 b + 0.5 tdev4 (+ the same), TSRR = -109.44 + 0.63 b + 0.37 tdev3, TSFC = -55.28 + 0.64 b + 0.35 mic2,
 * TSFL = 108.6 + 0.17 b + 0.83 mic1. thermalmonitord adds (5182 - PMU tcal) to the TP* readings (useTcalAdjust,
 * CommonProduct calibrationResistorTempDefault): PMU tcal is given as 51.82, no correction. The others are readings:
 * tdev1 cpu_therm, tdev5 wifi_therm, tjunc and the die sensors the SoC's tsens (pandora_cpu_tsens; the D22 table's
 * P sensors from cores 6-7, E from cores 0-5, G from the GPU; "Avg:" the mean, the others the highest).
 * AppleVTempDispatcher hands back what thermalmonitord computed (pandora_therm_ios_temp), logged every 10 s next to
 * Xiaomi's numbers.
 * thermalmonitord runs its control (timerCallBackHID -> updateSystemLoad) only in a round where every sensor of the
 * watchdog mask got a value, and requiresWatchDog defaults to true: all of the D22 table's HID sensors but mic2 and
 * Tarc are in it, and only services that exist give values (usb10: without TG0B, TG0V and NAND CH0 temp it read
 * the others all along and never computed). TG0B and TG0V (the gas gauge's battery temperature, a HID service on the
 * iPhone X too) carry the gauge's reading; NAND CH0 temp, information only, quiet_therm's (the main board).
 */
enum { PH_TDEV1, PH_TDEV2, PH_TDEV3, PH_TDEV4, PH_TDEV5, PH_TCAL, PH_MIC1, PH_MIC2, PH_BATT, PH_NAND, PH_INPUTS };
#define PH_DIE  (-1)     /* a die sensor: tsens group g, sensors first .. first + count - 1 */
#define PH_JUNC (-2)     /* the hottest of every tsens sensor */
static const struct {
	const char *name;
	int8_t input;
	uint8_t g, first, count, mean, callback;
} pt_hid[] = {
	{"PMU tdev1", PH_TDEV1, 0, 0, 0, 0, 0}, {"PMU tdev2", PH_TDEV2, 0, 0, 0, 0, 0}, {"PMU tdev3", PH_TDEV3, 0, 0, 0, 0, 0},
	{"PMU tdev4", PH_TDEV4, 0, 0, 0, 0, 0}, {"PMU tdev5", PH_TDEV5, 0, 0, 0, 0, 0}, {"PMU tcal", PH_TCAL, 0, 0, 0, 0, 0},
	{"PMU tjunc", PH_JUNC, 0, 0, 0, 0, 0}, {"mic1", PH_MIC1, 0, 0, 0, 0, 1}, {"mic2", PH_MIC2, 0, 0, 0, 0, 1},
	{"TG0B", PH_BATT, 0, 0, 0, 0, 0}, {"TG0V", PH_BATT, 0, 0, 0, 0, 0}, {"NAND CH0 temp", PH_NAND, 0, 0, 0, 0, 0},
	{"PMGR SOC Die Temp Sensor0", PH_DIE, 1, 0, 2, 0, 0}, {"Avg: PMGR SOC Die Temp Sensor0", PH_DIE, 1, 0, 2, 1, 0},
	{"Max: PMGR SOC Die Temp Sensor0", PH_DIE, 1, 0, 2, 0, 0}, {"PMGR SOC Die Temp Sensor1", PH_DIE, 1, 2, 2, 0, 0},
	{"Avg: PMGR SOC Die Temp Sensor1", PH_DIE, 1, 2, 2, 1, 0}, {"Max: PMGR SOC Die Temp Sensor1", PH_DIE, 1, 2, 2, 0, 0},
	{"PMGR SOC Die Temp Sensor2", PH_DIE, 2, 0, 6, 0, 0}, {"Avg: PMGR SOC Die Temp Sensor2", PH_DIE, 2, 0, 6, 1, 0},
	{"Max: PMGR SOC Die Temp Sensor2", PH_DIE, 2, 0, 6, 0, 0}, {"PMGR SOC Die Temp Sensor3", PH_DIE, 2, 6, 5, 0, 0},
	{"Avg: PMGR SOC Die Temp Sensor3", PH_DIE, 2, 6, 5, 1, 0}, {"Max: PMGR SOC Die Temp Sensor3", PH_DIE, 2, 6, 5, 0, 0},
	{"ACC Temp Sensor0", PH_DIE, 0, 0, 12, 0, 0}, {"Avg: ACC Temp Sensor0", PH_DIE, 0, 0, 12, 1, 0},
	{"Max: ACC Temp Sensor0", PH_DIE, 0, 0, 12, 0, 0}, {"ACC Temp Sensor1", PH_DIE, 1, 0, 2, 0, 0},
	{"Avg: ACC Temp Sensor1", PH_DIE, 1, 0, 2, 1, 0}, {"Max: ACC Temp Sensor1", PH_DIE, 1, 0, 2, 0, 0},
	{"ACC Temp Sensor2", PH_DIE, 1, 2, 2, 0, 0}, {"Avg: ACC Temp Sensor2", PH_DIE, 1, 2, 2, 1, 0},
	{"Max: ACC Temp Sensor2", PH_DIE, 1, 2, 2, 0, 0},
};
#define PT_HIDS (sizeof(pt_hid) / sizeof(pt_hid[0]))
static int32_t pt_hid_val[PH_INPUTS];     /* millidegrees, written by the thread */
static uint32_t pt_hid_ok;                /* bit per PH_* with a value */
static uint64_t pt_hid_reads;             /* copyEvent calls answered */

extern int32_t pandora_cpu_tsens(uint32_t g, uint32_t first, uint32_t count, int mean);   /* pandora_cpu.c */

/* For PandoraThermalHID.cpp. The count is 0 unless pandora_therm=2 or more and pandora_therm_ios=1: the
 * boot-args are read here, since IOKit matches the services before pandora_therm_init runs. 0263: off by
 * default. With the sensors there, thermalmonitord runs the iPhone X plist's backlight control, and from its
 * first round (about 24 s) CoreBrightness holds the screen at the lowest level and the slider does nothing
 * (usb10, usb11, spk7). On again with this phone's own plist (thermal step 2). */
unsigned pandora_therm_hid_count(void);
const char *pandora_therm_hid_name(unsigned i);
int pandora_therm_hid_callback(unsigned i);
int pandora_therm_hid_mdeg(unsigned i, int32_t *mdeg);

unsigned
pandora_therm_hid_count(void)
{
	int level = 0, ios = 0;
	(void)PE_parse_boot_argn("pandora_therm", &level, sizeof(level));
	(void)PE_parse_boot_argn("pandora_therm_ios", &ios, sizeof(ios));
	return level >= 2 && ios ? (unsigned)PT_HIDS : 0;
}

const char *
pandora_therm_hid_name(unsigned i)
{
	return i < PT_HIDS ? pt_hid[i].name : NULL;
}

int
pandora_therm_hid_callback(unsigned i)
{
	return i < PT_HIDS && pt_hid[i].callback;
}

/* Sensor i's temperature now; 0 when there is none yet. */
int
pandora_therm_hid_mdeg(unsigned i, int32_t *mdeg)
{
	if (i >= PT_HIDS) {
		return 0;
	}
	int32_t t = INT32_MIN;
	if (pt_hid[i].input == PH_DIE) {
		t = pandora_cpu_tsens(pt_hid[i].g, pt_hid[i].first, pt_hid[i].count, pt_hid[i].mean);
	} else if (pt_hid[i].input == PH_JUNC) {
		const int32_t a = pandora_cpu_tsens(0, 0, 12, 0), b = pandora_cpu_tsens(1, 0, 4, 0), c = pandora_cpu_tsens(2, 0, 11, 0);
		t = a > b ? a : b;
		t = c > t ? c : t;
	} else {
		const unsigned k = (unsigned)pt_hid[i].input;
		if (!(__atomic_load_n(&pt_hid_ok, __ATOMIC_ACQUIRE) & (1u << k))) {
			return 0;
		}
		*mdeg = __atomic_load_n(&pt_hid_val[k], __ATOMIC_RELAXED);
		__atomic_fetch_add(&pt_hid_reads, 1, __ATOMIC_RELAXED);
		return 1;
	}
	if (t == INT32_MIN) {
		return 0;
	}
	*mdeg = t * 100;    /* tsens counts tenths */
	__atomic_fetch_add(&pt_hid_reads, 1, __ATOMIC_RELAXED);
	return 1;
}

static void
pt_hid_set(unsigned k, int32_t mdeg)
{
	__atomic_store_n(&pt_hid_val[k], mdeg, __ATOMIC_RELAXED);
	__atomic_fetch_or(&pt_hid_ok, 1u << k, __ATOMIC_RELEASE);
}

/* One reading's values for iOS: in[] mi_thermald's inputs (in[5] the battery), vs[] its five virtual sensors
 * (front back left right top), all millidegrees. The five conversions invert the D22 formulas above. */
static void
pt_hid_update(const int32_t in[7], const int32_t vs[PT_VS], bool whole)
{
	pt_hid_set(PH_TCAL, 51820);
	if (!whole) {
		return;
	}
	const int64_t b = in[5], side = vs[2] > vs[3] ? vs[2] : vs[3];
	pt_hid_set(PH_TDEV1, in[6]);
	pt_hid_set(PH_TDEV5, in[4]);
	pt_hid_set(PH_BATT, in[5]);
	pt_hid_set(PH_NAND, in[3]);
	pt_hid_set(PH_TDEV2, (int32_t)((vs[1] - 2222 - 45 * b / 100) * 100 / 48));
	pt_hid_set(PH_TDEV4, (int32_t)((side + 802 - 50 * b / 100) * 100 / 50));
	pt_hid_set(PH_TDEV3, (int32_t)((vs[4] + 1094 - 63 * b / 100) * 100 / 37));
	pt_hid_set(PH_MIC2, (int32_t)((vs[4] + 553 - 64 * b / 100) * 100 / 35));
	pt_hid_set(PH_MIC1, (int32_t)((vs[0] - 1086 - 17 * b / 100) * 100 / 83));
}

/* What thermalmonitord computed, sent to AppleVTempDispatcher (PandoraThermalHID.cpp): its 4CC and the value in
 * hundredths of a degree. The last of each of up to 16 4CCs is kept for the log. */
void pandora_therm_ios_temp(const char *key, int32_t centi);
static struct {
	char key[5];
	int32_t centi;
} pt_ios[16];
static uint32_t pt_nios, pt_ios_sets;

void
pandora_therm_ios_temp(const char *key, int32_t centi)
{
	__atomic_fetch_add(&pt_ios_sets, 1, __ATOMIC_RELAXED);
	const uint32_t n = __atomic_load_n(&pt_nios, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < n; i++) {
		if (strncmp(pt_ios[i].key, key, 4) == 0) {
			__atomic_store_n(&pt_ios[i].centi, centi, __ATOMIC_RELAXED);
			return;
		}
	}
	if (n < 16) {
		strlcpy(pt_ios[n].key, key, sizeof(pt_ios[n].key));
		pt_ios[n].centi = centi;
		__atomic_store_n(&pt_nios, n + 1, __ATOMIC_RELEASE);
	}
}

static void
pt_ios_log(const int32_t vs[PT_VS])
{
	char line[400], b[12];
	size_t at = 0;
	const uint32_t n = __atomic_load_n(&pt_nios, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < n && at < sizeof(line); i++) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %s", pt_ios[i].key,
		    pt_fmt(b, sizeof(b), __atomic_load_n(&pt_ios[i].centi, __ATOMIC_RELAXED) * 10));
	}
	char f[PT_VS][12];
	printf("PANDORA THERM: iOS computed%s (%u values sent); Xiaomi front %s back %s left %s right %s top %s; "
	    "sensor reads %llu\n", n ? line : " nothing yet", __atomic_load_n(&pt_ios_sets, __ATOMIC_RELAXED),
	    pt_fmt(f[0], 12, vs[0]), pt_fmt(f[1], 12, vs[1]), pt_fmt(f[2], 12, vs[2]), pt_fmt(f[3], 12, vs[3]),
	    pt_fmt(f[4], 12, vs[4]), __atomic_load_n(&pt_hid_reads, __ATOMIC_RELAXED));
}

static int pt_level, pt_gpio_level;

static void
pt_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	pt_sleep(2000);
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
		bool measured = true;    /* the six inputs from the PMIC hold results */
		for (unsigned i = 0; i < PT_CHANS; i++) {
			const uint16_t code = pt_chan[i].sdam ? s1[pt_chan[i].slot] : s0[pt_chan[i].slot];
			changed += reads && code != last[i];
			last[i] = code;
			t[i] = pandora_ntc_mdeg(code);
		}
		for (unsigned k = 0; k < 7; k++) {
			if (pt_in[k] >= 0) {
				const uint16_t code = last[pt_in[k]];
				measured = measured && code != 0 && code != 0xffff;
			}
		}
		reads++;
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
		/* limits only from a whole reading: all six PMIC inputs and the gauge */
		const bool moved = pt_level >= 2 && measured && gauge && pt_limit(body);
		pt_hid_update(in, vs, measured && gauge);
		if (moved) {
			char b[12];
			printf("PANDORA THERM: body %s: limits cpu0 %u kHz, cpu6 %u kHz, gpu kgsl level %d (pandora_gpu level %u); "
			    "steps %d %d %d\n", pt_fmt(b, sizeof(b), body), pt_target(0), pt_target(1),
			    pt_step[2] < 0 ? -1 : (int)pt_target(2), __atomic_load_n(&pandora_gpu_body_top, __ATOMIC_RELAXED),
			    pt_step[0], pt_step[1], pt_step[2]);
		}
		if (n % PT_LOG_EVERY == 0 || n == 1) {
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
			if (pt_level >= 2) {
				pt_ios_log(vs);
			}
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
