def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0284: boot-arg pandora_cslog=NAME logs every code signature a process of that name adds -----------------
# aud1-aud4: mediaserverd's HAL activates our IOAudio2Device, but VirtualAudio (/Library/Audio/Plug-Ins/HAL/
# VirtualAudio.plugin, a legacy AudioHardwarePlugIn) never builds a virtual device and never logs, not even with its
# categories at debug; CoreAudio's HALS_PlugInManager loads only ".driver" bundles (9 loads = the 9 in /System).
# dyld adds the signature of every image it maps outside the shared cache (ubc_cs_blob_add): logging those for
# mediaserverd shows which plug-ins it loads, and whether VirtualAudio is among them.
P = 'bsd/kern/ubc_subr.c'
patch(P, '''int
ubc_cs_blob_add(
	struct vnode    *vp,''', '''#include <pexpert/pexpert.h>

/*
 * PANDORA (0284): boot-arg pandora_cslog=NAME logs each code signature a process of that name adds (the images
 * dyld maps outside the shared cache: plug-ins, bundles), with the result.
 */
static void
pandora_cslog(struct vnode *vp, int error)
{
	static int state;                       /* 0 not read yet, 1 off, 2 on */
	static char want[32];
	if (state == 0) {
		state = PE_parse_boot_arg_str("pandora_cslog", want, sizeof(want)) && want[0] ? 2 : 1;
	}
	if (state != 2) {
		return;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	if (strncmp(name, want, sizeof(name)) != 0) {
		return;
	}
	char path[256];
	int len = sizeof(path);
	if (vn_getpath(vp, path, &len) != 0) {
		strlcpy(path, "?", sizeof(path));
	}
	printf("PANDORA CS: %s adds the signature of %s: %d\\n", name, path, error);
}

int
ubc_cs_blob_add(
	struct vnode    *vp,''')
patch(P, '''out:
	if (error) {
		if (error != EAGAIN) {
			printf("check_signature[pid: %d]: error = %d\\n", proc_getpid(current_proc()), error);''', '''out:
	pandora_cslog(vp, error);
	if (error) {
		if (error != EAGAIN) {
			printf("check_signature[pid: %d]: error = %d\\n", proc_getpid(current_proc()), error);''')
print('patched (0284 pandora_cslog)')
