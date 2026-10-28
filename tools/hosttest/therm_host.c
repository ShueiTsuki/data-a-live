/*
 * Host test for wsl/pandora_ntc.h. The codes are SDAM slot words read on Android (thermadc raw sdam_t1.txt,
 * 2026-10-28), each next to the thermal zone Android showed for that sensor at the same moment
 * (zones_t1.txt); the mapping is the DT order of vadc@9000's thermal-monitor channels (wsl/pandora_therm.c).
 * The two readings were 1.4 s apart, so warming sensors differ a little: within 0.45 degree passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../../wsl/pandora_ntc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int
main(void)
{
	static const struct {
		const char *name;
		uint16_t code;
		int32_t android;
	} s[] = {
		{"xoagg", 26929, 33568}, {"pa2", 26753, 33783}, {"display", 29752, 29606}, {"charger", 26962, 33786},
		{"wireless", 26252, 34777}, {"cpu", 26440, 34545}, {"xo", 26952, 33530}, {"wifi", 27035, 33881},
		{"pa1", 26635, 33981}, {"quiet", 26967, 33486}, {"flash", 27758, 32372}, {"batt", 25818, 35228},
		{"conn", 30049, 29198},
	};
	for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
		const int32_t t = pandora_ntc_mdeg(s[i].code);
		printf("  %-9s code %5u -> %6d, Android %6d\n", s[i].name, s[i].code, t, s[i].android);
		CHECK(abs(t - s[i].android) <= 450);
	}
	/* the ends of the table and the code cap */
	CHECK(pandora_ntc_mdeg(0xffff) == -40 * 1024);
	CHECK(pandora_ntc_mdeg(0) == 127 * 1024);
	CHECK(pandora_ntc_mdeg(65000) == -40 * 1024);
	/* an exact table point: R(35 degrees) = 63176 ohm is code 63176 * 65535 / 163176 */
	CHECK(abs(pandora_ntc_mdeg(25373) - 35 * 1024) <= 2);
	printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
