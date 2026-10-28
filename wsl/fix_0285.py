def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0285: boot-arg pandora_pathlog=NAME logs the plug-in paths a process opens, accesses or stats -------------
# aud5 (k0284, pandora_cslog=mediaserverd): mediaserverd maps 8 ".driver" HAL plug-ins and never VirtualAudio.
# CoreAudio's HALS_System::StartServer scans /Library/Audio/Plug-Ins/HAL for ".plugin" bundles (access R|X, opendir,
# CFPlugInCreate, factories of type F8BB1C28-BAE8-11D6-9C31-00039315CD46, CFPlugInInstanceCreate). Logging
# mediaserverd's open/access/stat of paths with "Plug-Ins" or "VirtualAudio" shows how far it gets.
P = 'bsd/vfs/vfs_syscalls.c'
patch(P, '''static int
openat_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,
    int fd, enum uio_seg segflg, int *retval)
{''', '''#if defined(PANDORA)
/*
 * PANDORA (0285): boot-arg pandora_pathlog=NAME logs each open, access or stat by a process of that name of a path
 * containing "Plug-Ins" or "VirtualAudio" (which audio plug-ins mediaserverd looks at).
 */
static void
pandora_pathlog(const char *op, user_addr_t path, enum uio_seg segflg)
{
	static int state;                       /* 0 not read yet, 1 off, 2 on */
	static char want[32];
	if (state == 0) {
		state = PE_parse_boot_arg_str("pandora_pathlog", want, sizeof(want)) && want[0] ? 2 : 1;
	}
	if (state != 2) {
		return;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	if (strncmp(name, want, sizeof(name)) != 0) {
		return;
	}
	char p[256];
	size_t len = 0;
	int e = segflg == UIO_SYSSPACE ? copystr(CAST_DOWN(void *, path), p, sizeof(p), &len) :
	    copyinstr(path, p, sizeof(p), &len);
	if (e != 0 || (strnstr(p, "Plug-Ins", sizeof(p)) == NULL && strnstr(p, "VirtualAudio", sizeof(p)) == NULL)) {
		return;
	}
	printf("PANDORA PATH: %s %s %s\\n", name, op, p);
}
#endif /* PANDORA */

static int
openat_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,
    int fd, enum uio_seg segflg, int *retval)
{
#if defined(PANDORA)
	pandora_pathlog("open", path, segflg);
#endif''')
patch(P, '''static int
faccessat_internal(vfs_context_t ctx, int fd, user_addr_t path, int amode,
    int flag, enum uio_seg segflg)
{''', '''static int
faccessat_internal(vfs_context_t ctx, int fd, user_addr_t path, int amode,
    int flag, enum uio_seg segflg)
{
#if defined(PANDORA)
	pandora_pathlog("access", path, segflg);
#endif''')
patch(P, '''static int
fstatat_internal(vfs_context_t ctx, user_addr_t path, user_addr_t ub,
    user_addr_t xsecurity, user_addr_t xsecurity_size, int isstat64,
    enum uio_seg segflg, int fd, int flag)
{''', '''static int
fstatat_internal(vfs_context_t ctx, user_addr_t path, user_addr_t ub,
    user_addr_t xsecurity, user_addr_t xsecurity_size, int isstat64,
    enum uio_seg segflg, int fd, int flag)
{
#if defined(PANDORA)
	pandora_pathlog("stat", path, segflg);
#endif''')
print('patched (0285 pandora_pathlog)')
