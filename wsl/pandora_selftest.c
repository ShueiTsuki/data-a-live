/*
 * PANDORA input self-test (boot-arg pandora_selftest=SECONDS, default off): after that
 * many seconds the kernel itself presses keys and touches the screen through the same
 * entry points the drivers use (sweet_hid_key, sweet_hid_touch), and logs a screen
 * thumbnail (sweet_fb_thumb) before and after each step, so a run without anybody at the
 * phone shows how iOS reacted. Steps:
 *   1 thumbnail "start"
 *   2 power key press/release -> expect "SWEET FB: display off"; thumbnail "power1"
 *   3 power key again -> display on; thumbnail "power2"
 *   4 swipe up from the bottom edge (home gesture) -> thumbnail "swipeup"
 *   5 volume up, volume down presses (logged by SweetHIDButtons)
 *   6 swipe down from the top right corner (Control Center) -> thumbnail "cc"
 *   7 drag up along pandora_selftest_bx/by (brightness slider guess, logical pixels)
 *     -> CoreBrightness should write "brightness" (SWEET BL); thumbnail "bright"
 * Coordinates are in the presented 1125 x 2436 display (SweetHIDTouch on PANDORA).
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>

extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_key(int key, int down);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern void sweet_fb_thumb(const char *tag);
extern int sweet_fb_display_off;
void pandora_selftest_init(void);

static int st_chan;

static void
st_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&st_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
st_key(int key, const char *name)
{
	printf("PANDORA SELFTEST: %s press\n", name);
	sweet_hid_key(key, 1);
	st_sleep(120);
	sweet_hid_key(key, 0);
}

/* One finger from (x0,y0) to (x1,y1) in steps every 16 ms. */
static void
st_swipe(const char *name, int x0, int y0, int x1, int y1, unsigned steps)
{
	printf("PANDORA SELFTEST: %s %d,%d -> %d,%d\n", name, x0, y0, x1, y1);
	for (unsigned i = 0; i <= steps; i++) {
		int x = x0 + (x1 - x0) * (int)i / (int)steps;
		int y = y0 + (y1 - y0) * (int)i / (int)steps;
		sweet_hid_touch(0, 1, x, y);
		st_sleep(16);
	}
	sweet_hid_touch(0, 0, x1, y1);
}

static void
st_thread(void *arg, wait_result_t result)
{
	unsigned secs = 0, bx = 940, by0 = 1300, by1 = 700;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_selftest", &secs, sizeof(secs));
	(void)PE_parse_boot_argn("pandora_selftest_bx", &bx, sizeof(bx));
	(void)PE_parse_boot_argn("pandora_selftest_by0", &by0, sizeof(by0));
	(void)PE_parse_boot_argn("pandora_selftest_by1", &by1, sizeof(by1));
	printf("PANDORA SELFTEST: starting in %u s\n", secs);
	st_sleep(secs * 1000u);
	printf("PANDORA SELFTEST: display_off=%d\n", sweet_fb_display_off);
	sweet_fb_thumb("start");
	st_key(0, "power");
	st_sleep(3000);
	printf("PANDORA SELFTEST: after power 1 display_off=%d\n", sweet_fb_display_off);
	sweet_fb_thumb("power1");
	st_key(0, "power");
	st_sleep(3000);
	printf("PANDORA SELFTEST: after power 2 display_off=%d\n", sweet_fb_display_off);
	sweet_fb_thumb("power2");
	st_swipe("swipe up", 562, 2425, 562, 1100, 20);
	st_sleep(3000);
	sweet_fb_thumb("swipeup");
	st_key(1, "volume up");
	st_sleep(1500);
	st_key(2, "volume down");
	st_sleep(1500);
	st_swipe("control center", 1000, 4, 1000, 1300, 20);
	st_sleep(2500);
	sweet_fb_thumb("cc");
	st_swipe("brightness drag", (int)bx, (int)by0, (int)bx, (int)by1, 30);
	st_sleep(2500);
	sweet_fb_thumb("bright");
	printf("PANDORA SELFTEST: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_selftest_init(void)
{
	thread_t thread;
	unsigned secs = 0;
	if (!PE_parse_boot_argn("pandora_selftest", &secs, sizeof(secs)) || secs == 0) {
		return;
	}
	if (kernel_thread_start(st_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
