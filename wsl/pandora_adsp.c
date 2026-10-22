/*
 * PANDORA audio DSP (ADSP) bring-up, step 1: talk to TrustZone. Boot-arg pandora_adsp=1.
 * SCM calls as Linux drivers/firmware/qcom/qcom_scm-smc.c: SMCCC yielding call, SMC64,
 * owner SIP, function (svc << 8) | cmd, x1 = arginfo (argument count | 2-bit types),
 * x2..x5 arguments; x0 = 1 (interrupted) means call again with x0 = 1 and the returned
 * x6. Only queries here:
 *   INFO_IS_CALL_AVAIL (svc 6 cmd 1) for the PIL calls and SHM bridge enable
 *   PIL PAS_IS_SUPPORTED (svc 2 cmd 7) for pas 1 (ADSP) and 36 (ADSP DTB, canoe)
 *   MP SHM_BRIDGE_ENABLE (svc 0xc cmd 0x1c): what Linux calls at probe
 * Plan for the rest: PLAN.md "扬声器路线".
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>

extern kern_return_t thread_terminate(thread_t);
void pandora_adsp_init(void);

struct scm_res {
	uint64_t a0, a1, a2, a3;
};

static void
scm_smc(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
    struct scm_res *res)
{
	uint64_t a0 = fn, a6 = 0;
	for (int tries = 0; tries < 1000; tries++) {
		register uint64_t r0 __asm__("x0") = a0;
		register uint64_t r1 __asm__("x1") = arginfo;
		register uint64_t r2 __asm__("x2") = x2;
		register uint64_t r3 __asm__("x3") = x3;
		register uint64_t r4 __asm__("x4") = x4;
		register uint64_t r5 __asm__("x5") = x5;
		register uint64_t r6 __asm__("x6") = a6;
		register uint64_t r7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0"
		    : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		res->a0 = r0;
		res->a1 = r1;
		res->a2 = r2;
		res->a3 = r3;
		if (r0 != 1) {           /* QCOM_SCM_INTERRUPTED */
			return;
		}
		a0 = 1;
		a6 = r6;
	}
}

#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
#define SCM_ARGS(n)       ((uint64_t)(n) & 0xf)

static int
scm_call_avail(unsigned svc, unsigned cmd)
{
	struct scm_res r;
	uint64_t id = (2ULL << 24) | ((svc & 0xff) << 8) | (cmd & 0xff);
	scm_smc(SCM_FN(6, 1), SCM_ARGS(1), id, 0, 0, 0, &r);
	printf("PANDORA ADSP: is_call_avail svc %#x cmd %#x -> a0 %#llx a1 %#llx\n", svc, cmd, r.a0, r.a1);
	return r.a0 == 0 ? (int)r.a1 : -1;
}

static void
adsp_thread(void *arg, wait_result_t result)
{
	struct scm_res r;
	(void)arg;
	(void)result;
	printf("PANDORA ADSP: SCM probe\n");
	(void)scm_call_avail(2, 1);      /* PAS_INIT_IMAGE */
	(void)scm_call_avail(2, 2);      /* PAS_MEM_SETUP */
	(void)scm_call_avail(2, 5);      /* PAS_AUTH_AND_RESET */
	(void)scm_call_avail(2, 7);      /* PAS_IS_SUPPORTED */
	(void)scm_call_avail(0xc, 0x1c); /* SHM_BRIDGE_ENABLE */
	(void)scm_call_avail(0xc, 0x1e); /* SHM_BRIDGE_CREATE */
	static const unsigned pas[] = {1, 36};
	for (unsigned i = 0; i < 2; i++) {
		scm_smc(SCM_FN(2, 7), SCM_ARGS(1), pas[i], 0, 0, 0, &r);
		printf("PANDORA ADSP: pas_is_supported(%u) -> a0 %#llx a1 %#llx\n", pas[i], r.a0, r.a1);
	}
	scm_smc(SCM_FN(0xc, 0x1c), SCM_ARGS(0), 0, 0, 0, 0, &r);
	printf("PANDORA ADSP: shm_bridge_enable -> a0 %#llx a1 %#llx\n", r.a0, r.a1);
	printf("PANDORA ADSP: probe done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_adsp_init(void)
{
	thread_t thread;
	int on = 0;
	(void)PE_parse_boot_argn("pandora_adsp", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(adsp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
