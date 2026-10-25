/*
 * PANDORA 0224: the home screen swiped by itself, for unattended tests (boot-arg pandora_autoswipe=SECONDS; off
 * without it), as sweethidtest=N presses Home by itself: the sampling profiler (pandora_prof) needs the screen moving
 * while nobody holds the phone. From pandora_autoswipe_at seconds of uptime (default 45; backboardd draws its first
 * frame at about 20 s): the side button when the display is off, a swipe up from the bottom edge (unlock; the test
 * phone has no passcode), then the home screen paged left and right, a swipe about every 0.8 s, the swipe up again
 * every 20 swipes in case the lock screen came back, for SECONDS. Finger 0 through sweet_hid_touch, in the presented
 * display's pixels (1125 x 2436) as bsd/dev/pandora_touch.c delivers them.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>

extern void sweet_hid_touch(int finger, int down, int x, int y);
extern void sweet_hid_key(int key, int down);           /* 0 side button */
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);
extern int sweet_fb_display_off;       /* iokit/Kernel/arm/SweetFramebuffer.cpp */
void pandora_autoswipe_start(void);

#define AS_W            1125
#define AS_H            2436
#define AS_STEPS        12              /* moves of one swipe */
#define AS_STEP_MS      16              /* one 60 Hz touch report apart */
#define AS_PAUSE_MS     600             /* after a swipe: the page settles */
#define AS_PRIORITY     92              /* BASEPRI_PREEMPT, as pandora_touch's thread */

static int as_chan;
static uint32_t as_secs, as_at = 45;

static void
as_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&as_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
as_uptime_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* Down at (x0, y0), AS_STEPS moves to (x1, y1), up there. */
static void
as_swipe(int x0, int y0, int x1, int y1)
{
	sweet_hid_touch(0, 1, x0, y0);
	for (int i = 1; i <= AS_STEPS; i++) {
		as_sleep(AS_STEP_MS);
		sweet_hid_touch(0, 1, x0 + (x1 - x0) * i / AS_STEPS, y0 + (y1 - y0) * i / AS_STEPS);
	}
	as_sleep(AS_STEP_MS);
	sweet_hid_touch(0, 0, x1, y1);
	as_sleep(AS_PAUSE_MS);
}

/* The display on (side button when it is off), then a swipe up from the bottom edge. */
static void
as_unlock(void)
{
	if (sweet_fb_display_off) {
		sweet_hid_key(0, 1);
		as_sleep(100);
		sweet_hid_key(0, 0);
		as_sleep(1500);
	}
	as_swipe(AS_W / 2, AS_H - 6, AS_W / 2, AS_H * 3 / 5);
}

static void
as_thread(__unused void *arg, __unused wait_result_t wr)
{
	const uint64_t start = (uint64_t)as_at * 1000;
	while (as_uptime_ms() < start) {
		as_sleep(500);
	}
	printf("PANDORA AUTOSWIPE: swiping the home screen for %u s\n", as_secs);
	const uint64_t end = as_uptime_ms() + (uint64_t)as_secs * 1000;
	unsigned swipes = 0;
	while (as_uptime_ms() < end) {
		if (swipes % 20 == 0) {
			as_unlock();
		}
		const int y = AS_H / 2, left = AS_W * 3 / 20, right = AS_W * 17 / 20;
		if (swipes & 1) {
			as_swipe(left, y, right, y);         /* back a page */
		} else {
			as_swipe(right, y, left, y);         /* next page */
		}
		if (++swipes % 50 == 0) {
			printf("PANDORA AUTOSWIPE: %u swipes\n", swipes);
		}
	}
	printf("PANDORA AUTOSWIPE: done, %u swipes\n", swipes);
}

void
pandora_autoswipe_start(void)
{
	if (!PE_parse_boot_argn("pandora_autoswipe", &as_secs, sizeof(as_secs)) || !as_secs) {
		return;
	}
	(void)PE_parse_boot_argn("pandora_autoswipe_at", &as_at, sizeof(as_at));
	thread_t thread;
	if (kernel_thread_start_priority(as_thread, NULL, AS_PRIORITY, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
