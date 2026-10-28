/*
 * A board thermistor's ADC code to the temperature Android reports, as qcom_vadc7_scale_hw_calib_therm
 * (Android qcom-vadc-common.ko 0x29c; Linux qcom-vadc-common.c): the code is capped at 0xfff7, the
 * resistance is 100 kOhm * code / (65535 - code) (pull-up, full scale of adc5_gen4_data_pmic +0x10),
 * looked up in the NTC table and interpolated linearly. The table counts 1/1024 degree and Android
 * hands that number on as millidegrees, so 1024 here is "1 degree" for Android's thermal settings.
 * Pure, for bsd/dev/pandora_therm.c and tools/hosttest.
 */
#ifndef PANDORA_NTC_H
#define PANDORA_NTC_H

#include <stdint.h>
#include "pandora_ntc_table.h"

static inline int32_t
pandora_ntc_mdeg(uint16_t code)
{
	const uint32_t c = code < 0xfff7u ? code : 0xfff7u;
	uint64_t r = (uint64_t)c * 100000u / (65535u - c);
	if (r > 0x7fffffffu) {
		r = 0x7fffffffu;
	}
	unsigned i = 0;
	while (i < 168 && pt_ntc_ohm[i] > r) {
		i++;
	}
	if (i == 0) {
		return -40 * 1024;
	}
	if (i == 168) {
		return 127 * 1024;
	}
	/* T(i-1) + 1024 * (R - R(i-1)) / (R(i) - R(i-1)), 32-bit as the driver (0x394-0x39c) */
	const int32_t prev = ((int32_t)i - 41) * 1024;
	if ((uint32_t)r == pt_ntc_ohm[i]) {
		return prev + 1024;
	}
	return prev + (int32_t)(1024 * ((int32_t)r - (int32_t)pt_ntc_ohm[i - 1])) /
	       ((int32_t)pt_ntc_ohm[i] - (int32_t)pt_ntc_ohm[i - 1]);
}

#endif
