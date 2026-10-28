def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0274: a program that keeps crashing is not started again this boot ----------------------------------------------
# User: 「能不能弄失败不重新拉起」. spk16, one 90 s boot: AegirPoster crashed 10 times, SiriTTSSynthesizer,
# MacinTalkAUSP and KonaSynthesizer 6 each (no audio device yet), assetsd 3, and every crash ran ReportCrash (up to
# 45 % of a core); launchd started each one again. Now the crash hook in kern_exit.c (where Crash Reporter is told)
# counts crashes per program, and from the third (boot-arg sweetcrashstop=N, 0 = off) its exec fails before dyld
# the way a sweetnoexec one does, until the next boot. Never stopped: the programs without which the phone is not
# usable. Names compared on their first MAXCOMLEN bytes (p_comm).
X = 'bsd/kern/kern_exec.c'
edit(X, '''#include "sweet_execswap.h"
static LCK_MTX_DECLARE(sweet_execswap_lock, &proc_lck_grp);
''', '''/*
 * sweetcrashstop=N (0274; default 3, 0 off): a program that crashed N times this boot (kern_exit.c, where Crash
 * Reporter is told) is not started again until the next boot; launchd's next spawn fails here like a sweetnoexec
 * one. Never stopped: the programs without which the phone is not usable (better they restart).
 */
#define SWEET_CRASH_SLOTS 48
static LCK_MTX_DECLARE(sweet_crash_lock, &proc_lck_grp);
static struct {
\tchar name[MAXCOMLEN + 1];
\tuint32_t n;
\tbool stopped;
} sweet_crashes[SWEET_CRASH_SLOTS];
static int sweet_crash_limit = -1;
static const char *const sweet_crash_keep[] = {"launchd", "SpringBoard", "backboardd", "runningboardd", "logd",
    "notifyd", "configd", "securityd", "keybagd", "ReportCrash", "mediaserverd", "audiomxd", "lsd", "installd",
    "UserEventAgent", "cfprefsd", "containermanager"};

static int
sweet_crash_limit_get(void)
{
\tif (sweet_crash_limit < 0) {
\t\tint n = 3;
\t\t(void)PE_parse_boot_argn("sweetcrashstop", &n, sizeof(n));
\t\tsweet_crash_limit = n < 0 ? 0 : n;
\t}
\treturn sweet_crash_limit;
}

void sweet_crash_count(const char *name);
void
sweet_crash_count(const char *name)
{
\tconst int limit = sweet_crash_limit_get();
\tif (!limit || name == NULL || !name[0]) {
\t\treturn;
\t}
\tfor (unsigned k = 0; k < sizeof(sweet_crash_keep) / sizeof(sweet_crash_keep[0]); k++) {
\t\tif (strncmp(name, sweet_crash_keep[k], MAXCOMLEN) == 0) {
\t\t\treturn;
\t\t}
\t}
\tlck_mtx_lock(&sweet_crash_lock);
\tunsigned i = 0;
\tfor (; i < SWEET_CRASH_SLOTS && sweet_crashes[i].name[0]; i++) {
\t\tif (strncmp(sweet_crashes[i].name, name, MAXCOMLEN) == 0) {
\t\t\tbreak;
\t\t}
\t}
\tif (i < SWEET_CRASH_SLOTS) {
\t\tif (!sweet_crashes[i].name[0]) {
\t\t\tstrlcpy(sweet_crashes[i].name, name, sizeof(sweet_crashes[i].name));
\t\t}
\t\tif (++sweet_crashes[i].n == (uint32_t)limit) {
\t\t\tsweet_crashes[i].stopped = true;
\t\t\tprintf("CRASH %s: %u crashes this boot, not started again (sweetcrashstop)\\n", sweet_crashes[i].name,
\t\t\t    sweet_crashes[i].n);
\t\t}
\t}
\tlck_mtx_unlock(&sweet_crash_lock);
}

static bool
sweet_crash_stopped(const char *name)
{
\tbool stopped = false;
\tif (!sweet_crash_limit_get()) {
\t\treturn false;
\t}
\tlck_mtx_lock(&sweet_crash_lock);
\tfor (unsigned i = 0; i < SWEET_CRASH_SLOTS && sweet_crashes[i].name[0]; i++) {
\t\tif (sweet_crashes[i].stopped && strncmp(sweet_crashes[i].name, name, MAXCOMLEN) == 0) {
\t\t\tstopped = true;
\t\t\tbreak;
\t\t}
\t}
\tlck_mtx_unlock(&sweet_crash_lock);
\treturn stopped;
}

#include "sweet_execswap.h"
static LCK_MTX_DECLARE(sweet_execswap_lock, &proc_lck_grp);
''')
edit(X, '''\t\t\tif (!(imgp->ip_flags & IMGPF_SWEET_EXECSWAP) && sweet_exec_blocked(p->p_name, true)) {''',
     '''\t\t\tif (!(imgp->ip_flags & IMGPF_SWEET_EXECSWAP) &&
\t\t\t    (sweet_exec_blocked(p->p_name, true) || sweet_crash_stopped(p->p_name))) {''')

E = 'bsd/kern/kern_exit.c'
edit(E, '''\t\t\textern void sweet_crash_log(const char *, int, int, uint64_t, uint64_t);
\t\t\tsweet_crash_log(p->p_comm, proc_getpid(p), WTERMSIG(rv),''', '''\t\t\textern void sweet_crash_log(const char *, int, int, uint64_t, uint64_t);
\t\t\textern void sweet_crash_count(const char *);      /* kern_exec.c, 0274 */
\t\t\tsweet_crash_count(p->p_comm);
\t\t\tsweet_crash_log(p->p_comm, proc_getpid(p), WTERMSIG(rv),''')
print('patched (0274 sweetcrashstop)')
