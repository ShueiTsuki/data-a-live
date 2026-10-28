W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0286: the file installer renames ("> FROM TO" in files.txt) -----------------------------------------------
# aud6 (k0285, pandora_pathlog=mediaserverd): HALS_System::StartServer opens /Library/Audio/Plug-Ins/HAL and finds
# nothing: on the phone that folder is empty and VirtualAudio.plugin sits in HAL.disabled, moved there by ios-sweet's
# root image v12 (sweet-root-tweaks.py audio(): mediaserverd crashed in VirtualAudio while there was no audio
# device). Without VirtualAudio there is no virtual audio device at all. The installer can now move it back.
copy('pandora_files.c', 'bsd/dev/pandora_files.c')
patch('bsd/vfs/vfs_syscalls.c', '''	return mkdir1at(vfs_context_kernel(), CAST_USER_ADDR_T(path), &va, AT_FDCWD, UIO_SYSSPACE);
}
#endif /* PANDORA */''', '''	return mkdir1at(vfs_context_kernel(), CAST_USER_ADDR_T(path), &va, AT_FDCWD, UIO_SYSSPACE);
}

/* For bsd/dev/pandora_files.c: rename of kernel-space paths as the rename syscall does it (patch 0286). */
int pandora_rename(const char *from, const char *to);

int
pandora_rename(const char *from, const char *to)
{
	return renameat_internal(vfs_context_kernel(), AT_FDCWD, CAST_USER_ADDR_T(from), AT_FDCWD,
	           CAST_USER_ADDR_T(to), UIO_SYSSPACE, 0);
}
#endif /* PANDORA */''')
print('patched (0286 installer rename)')
