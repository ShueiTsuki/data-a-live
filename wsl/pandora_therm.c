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
void pandora_therm_init(void);

#define PT_SDAM0     0x090u      /* PMK8850 (SID 0) vadc@9000 */
#define PT_SDAM1     0x091u      /* vadc@9000's second SDAM (DT reg <0x9000 0x9100>, #size-cells 0) */
#define PT_DATA      0x5cu
#define PT_PERIOD_MS 1000u
#define PT_LOG_EVERY 10u

static const struct {
	const char *name;
	uint8_t sdam, slot;
} pt_chan[] = {
	{"xoagg", 0, 1}, {"qtm", 0, 2}, {"pa2", 0, 3}, {"display", 0, 4}, {"charger", 0, 5}, {"wireless", 0, 6},
	{"cpu", 0, 7}, {"xo", 1, 0}, {"wifi", 1, 1}, {"pa1", 1, 2}, {"quiet", 1, 3}, {"flash", 1, 4},
	{"batt", 1, 5}, {"conn", 1, 6},
};
#define PT_CHANS (sizeof(pt_chan) / sizeof(pt_chan[0]))

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

static void
pt_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	pt_sleep(10000);
	pt_dump_config(PT_SDAM0);
	pt_dump_config(PT_SDAM1);
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
	int on = 0;
	(void)PE_parse_boot_argn("pandora_therm", &on, sizeof(on));
	if (!on) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(pt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
