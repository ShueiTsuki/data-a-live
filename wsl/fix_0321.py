def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# ---- 0321: log work interval creation ------------------------------------------------------------------------------
# CoreAnimation's render server reports each frame's start, finish and deadline through a work interval (QuartzCore:
# CA::WindowServer::IOMFBDisplay::work_interval creates it with work_interval_create, begin_work_interval skips every
# instance call when that failed). The performance controller (osfmk/arm/pandora_perfctl.c) counted no work interval
# call in post15 and post16, so either nothing creates one or the kernel refuses (PRIV_WORK_INTERVAL, a thread already
# in a work interval). work_interval_ctl now logs every create with its flags and result and other failures.
F = 'bsd/kern/sys_work_interval.c'
sub(F,
    '#include <libkern/libkern.h>\n',
    '#include <libkern/libkern.h>\n'
    '#if defined(PANDORA)\n'
    '#include <sys/proc.h>\n'
    '#include <sys/systm.h>\n'
    '#include <os/atomic_private.h>\n'
    '#endif\n')
sub(F,
    'int\nwork_interval_ctl(__unused proc_t p, struct work_interval_ctl_args *uap,\n    __unused int32_t *retval)\n{\n',
    'static int\nwork_interval_ctl_inner(__unused proc_t p, struct work_interval_ctl_args *uap,\n'
    '    __unused int32_t *retval)\n{\n')
s = open(F).read()
s = s.rstrip('\n') + '''

#if defined(PANDORA)
/*
 * 0321: which processes create work intervals, with what flags, and what they get: every create, the first 200;
 * other operations only when they fail, the first 100. CoreAnimation's render server should create one per display
 * (CA_RENDER_SERVER with a thread group, 0x20000002) and report each frame through it.
 */
static uint32_t pandora_wi_creates, pandora_wi_fails;
#endif

int
work_interval_ctl(proc_t p, struct work_interval_ctl_args *uap, int32_t *retval)
{
	const int error = work_interval_ctl_inner(p, uap, retval);
#if defined(PANDORA)
	char name[32];
	if (uap->operation == WORK_INTERVAL_OPERATION_CREATE2 || uap->operation == WORK_INTERVAL_OPERATION_CREATE) {
		if (os_atomic_inc(&pandora_wi_creates, relaxed) <= 200) {
			struct work_interval_create_params params = {};
			if (uap->arg != USER_ADDR_NULL && uap->len >= sizeof(params)) {
				(void)copyin(uap->arg, &params, sizeof(params));
			}
			proc_selfname(name, sizeof(name));
			printf("PANDORA WI: %s[%d] tid %llu create op %u flags %#x -> %d (id %llu port %#x)\\n", name,
			    proc_selfpid(), thread_tid(current_thread()), uap->operation, params.wicp_create_flags, error,
			    params.wicp_id, params.wicp_port);
		}
	} else if (error != 0 && os_atomic_inc(&pandora_wi_fails, relaxed) <= 100) {
		proc_selfname(name, sizeof(name));
		printf("PANDORA WI: %s[%d] op %u id %#llx -> %d\\n", name, proc_selfpid(), uap->operation,
		    uap->work_interval_id, error);
	}
#endif
	return error;
}
'''
open(F, 'w').write(s)
