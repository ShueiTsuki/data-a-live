W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# The file installer makes directories ("- MODE PATH" lines in files.txt, bsd/dev/pandora_files.c)
# through pandora_mkdir, the mkdir syscall's mkdir1at on a kernel-space path (bsd/vfs/vfs_syscalls.c).
open('bsd/dev/pandora_files.c', 'w').write(open(W + 'pandora_files.c').read().replace('\r', ''))
p = 'bsd/vfs/vfs_syscalls.c'
s = open(p).read()
assert 'pandora_mkdir' not in s and s.count('\nmkdir1at(vfs_context_t ctx, user_addr_t path, struct vnode_attr *vap, int fd,') == 1
s += '''
#if defined(PANDORA)
/* For bsd/dev/pandora_files.c: mkdir of a kernel-space path as the mkdir syscall makes it (patch 0185). */
int pandora_mkdir(const char *path, int mode);

int
pandora_mkdir(const char *path, int mode)
{
	struct vnode_attr va;

	VATTR_INIT(&va);
	VATTR_SET(&va, va_mode, mode & ACCESSPERMS);
	return mkdir1at(vfs_context_kernel(), CAST_USER_ADDR_T(path), &va, AT_FDCWD, UIO_SYSSPACE);
}
#endif /* PANDORA */
'''
open(p, 'w').write(s)
print('patched')
