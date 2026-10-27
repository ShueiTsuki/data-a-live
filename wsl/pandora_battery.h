/*
 * The battery readings bsd/dev/pandora_adsp.c takes from the SOCCP's battery manager (PMIC
 * GLINK), for iokit/Kernel/arm/SweetBattery.cpp.
 */
#ifndef PANDORA_BATTERY_H
#define PANDORA_BATTERY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct pandora_battery {
	int valid;                              /* the readings below are in use */
	int present, external, charging, fully_charged;
	int percent;                            /* 0..100 */
	int voltage_mv, current_ma;             /* current: positive while the battery takes charge */
	int temperature, temperature_ok;        /* hundredths of a degree Celsius */
	int raw_mah, full_mah, design_mah, cycles;      /* 0: unknown */
	int time_to_full_min, time_to_empty_min;        /* -1: unknown */
};

/* The latest readings into b (size: sizeof(*b)); false when there are none to use. */
bool pandora_battery_get(struct pandora_battery *b, size_t size);

/* The bq27z561 gauge's standard commands as last read (bsd/dev/pandora_amp.c, pandora_gauge=3). */
struct pandora_gauge {
	int valid;
	int voltage_mv, current_ma;             /* current: the gauge's sign, positive while charging */
	int temperature_dk;                     /* 0.1 K */
	int soc, remaining_mah, full_mah, design_mah, cycles, health;
	int status;                             /* BatteryStatus (0x0a) */
	uint64_t at;                            /* mach_absolute_time of the read */
};

/* The latest gauge reading into g (size: sizeof(*g)); false when there is none. */
bool pandora_gauge_get(struct pandora_gauge *g, size_t size);

/* Read the gauge now instead of at the next period (charger plugged or unplugged). */
void pandora_gauge_kick(void);

/* iokit/Kernel/arm/SweetBattery.cpp: take pandora_battery_get's readings now, not at its next poll. */
void pandora_battery_changed(void);

#ifdef __cplusplus
}
#endif
#endif
