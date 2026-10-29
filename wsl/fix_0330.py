def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# ---- 0330: a daemon the kernel no longer starts runs as /usr/libexec/pandora_sink --------------------------------
# post25: the Settings Notifications page waited for good on CommCenter (_CTServerConnectionCopyCellBroadcastAlert-
# TypesForAllSIMSlots, a sync XPC call in viewDidLoad): CommCenter crashed 3 times, sweetcrashstop then failed each
# of launchd's spawns, and its services stayed registered with their requests queued. tools/sink/pandora_sink.c checks
# in the job's services and answers each request at once with an error.
F = 'bsd/kern/kern_exec.c'

sub(F, '''#include "sweet_execswap.h"
static LCK_MTX_DECLARE(sweet_execswap_lock, &proc_lck_grp);
static const char *
sweet_exec_swap(const char *path, int pid)
{
''', '''/*
 * sweetsink (0330; default 1, 0 off, 2 also for sweetnoexec): a program sweetcrashstop stopped is run as
 * /usr/libexec/pandora_sink when that file exists, with its own argv and environment (tools/sink/pandora_sink.c:
 * it checks in the launchd job's Mach services and answers every request at once with an error). Before, its
 * services stayed registered with launchd and requests to them waited for good: the Settings Notifications page
 * on CommCenter (post25). Not when the sink itself was stopped. The first 16 are logged.
 */
#define SWEET_SINK_PATH "/usr/libexec/pandora_sink"
static int sweet_sink_mode = -1;
static unsigned sweet_sink_logged;

static const char *
sweet_exec_sink(const char *path, int pid)
{
	if (sweet_sink_mode < 0) {
		int mode = 1;
		(void)PE_parse_boot_argn("sweetsink", &mode, sizeof(mode));
		sweet_sink_mode = mode;
	}
	if (sweet_sink_mode <= 0 || path == NULL) {
		return NULL;
	}
	const char *slash = strrchr(path, '/');
	char name[MAXCOMLEN + 1];
	strlcpy(name, slash ? slash + 1 : path, sizeof(name));
	if (!name[0] || strncmp(name, "pandora_sink", MAXCOMLEN) == 0 || sweet_crash_stopped("pandora_sink")) {
		return NULL;
	}
	const bool stopped = sweet_crash_stopped(name);
	if (!stopped && !(sweet_sink_mode >= 2 && sweet_exec_blocked(name, false))) {
		return NULL;
	}
	vnode_t vp = NULLVP;
	if (vnode_lookup(SWEET_SINK_PATH, 0, &vp, vfs_context_current()) != 0) {
		return NULL;
	}
	vnode_put(vp);
	if (sweet_sink_logged < 16) {
		sweet_sink_logged++;
		printf("SWEET SINK: %s pid %d runs as %s (%s)\\n", name, pid, SWEET_SINK_PATH,
		    stopped ? "sweetcrashstop" : "sweetnoexec");
	}
	return SWEET_SINK_PATH;
}

#include "sweet_execswap.h"
static LCK_MTX_DECLARE(sweet_execswap_lock, &proc_lck_grp);
static const char *
sweet_exec_swap(const char *path, int pid)
{
''')

sub(F, ''' const char *replacement = sweet_exec_swap(excpath, proc_getpid(p));
 if (replacement) {''', ''' const char *replacement = sweet_exec_swap(excpath, proc_getpid(p));
 if (!replacement) {
  replacement = sweet_exec_sink(excpath, proc_getpid(p));   /* 0330 */
 }
 if (replacement) {''')
