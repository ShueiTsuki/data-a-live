/*
 * The battery readings bsd/dev/pandora_adsp.c takes from the SOCCP's battery manager (PMIC
 * GLINK), for iokit/Kernel/arm/SweetBattery.cpp.
 */
#ifndef PANDORA_BATTERY_H
#define PANDORA_BATTERY_H
#include <stdbool.h>
#include <stddef.h>
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

#ifdef __cplusplus
}
#endif
#endif
