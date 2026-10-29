def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# ---- 0323: pandora_pathlog=NAME:TEXT logs failed opens and mkdirs -----------------------------------------------
# post16: contextstored's writes to Biome streams fail (BMStoreSource.sendEvent, then CoreDuet's "Simulating crash",
# its reason private), the App.InFocus stream (app usage) was never created, so dasd's widget budget is 0/0 = NaN and
# it asks coreduetd for predictions over months of 15-minute slots (about 3900 queries a boot, coreduetd at 76%).
# biomed, which opens Biome files for its clients, logs "No such file or directory" without the path. With
# NAME:TEXT the kernel logs each failed open (plain or data-protected) or mkdir of process NAME whose path contains
# TEXT, or whose name is relative to a directory whose path does, with the directory and the error.
F = 'bsd/vfs/vfs_syscalls.c'

sub(F,
    '/*\n * Go through the data-protected atomically controlled open (2)\n',
    '#if defined(PANDORA)\n'
    '/*\n'
    ' * PANDORA (0323): boot-arg pandora_pathlog=NAME:TEXT logs each open (plain or data-protected) or mkdir of a\n'
    ' * process named NAME that fails on a path containing TEXT, or on a name relative to a directory whose path\n'
    ' * contains it, with the directory, the flags, the protection class and the error; the first 400. (Without\n'
    ' * ":TEXT" pandora_pathlog keeps 0285\'s meaning below.) A mkdir of something that exists is not logged.\n'
    ' */\n'
    'static void\n'
    'pandora_pathlog_failed(vfs_context_t ctx, const char *op, int fd, user_addr_t path, enum uio_seg segflg,\n'
    '    int flags, int class, int error)\n'
    '{\n'
    '\tstatic int state;                       /* 0 not read yet, 1 off, 2 on */\n'
    '\tstatic char want[32], text[32];\n'
    '\tstatic uint32_t lines;\n'
    '\tif (error == 0) {\n'
    '\t\treturn;\n'
    '\t}\n'
    '\tif (state == 0) {\n'
    '\t\tchar arg[64];\n'
    '\t\tstate = 1;\n'
    '\t\tif (PE_parse_boot_arg_str("pandora_pathlog", arg, sizeof(arg))) {\n'
    '\t\t\tfor (size_t i = 0; arg[i] != 0 && i < sizeof(want) - 1; i++) {\n'
    '\t\t\t\tif (arg[i] == \':\') {\n'
    '\t\t\t\t\tmemcpy(want, arg, i);\n'
    '\t\t\t\t\tstrlcpy(text, arg + i + 1, sizeof(text));\n'
    '\t\t\t\t\tstate = want[0] && text[0] ? 2 : 1;\n'
    '\t\t\t\t\tbreak;\n'
    '\t\t\t\t}\n'
    '\t\t\t}\n'
    '\t\t}\n'
    '\t}\n'
    '\tif (state != 2 || lines >= 400) {\n'
    '\t\treturn;\n'
    '\t}\n'
    '\tchar name[32];\n'
    '\tproc_selfname(name, sizeof(name));\n'
    '\tif (strncmp(name, want, sizeof(name)) != 0) {\n'
    '\t\treturn;\n'
    '\t}\n'
    '\tchar p[256];\n'
    '\tsize_t len = 0;\n'
    '\tif ((segflg == UIO_SYSSPACE ? copystr(CAST_DOWN(void *, path), p, sizeof(p), &len) :\n'
    '\t    copyinstr(path, p, sizeof(p), &len)) != 0) {\n'
    '\t\treturn;\n'
    '\t}\n'
    '\tchar dir[256] = "";\n'
    '\tif (p[0] != \'/\' && fd != AT_FDCWD) {\n'
    '\t\tvnode_t dvp = NULLVP;\n'
    '\t\tif (vnode_getfromfd(ctx, fd, &dvp) == 0) {\n'
    '\t\t\tint dlen = sizeof(dir);\n'
    '\t\t\tif (vn_getpath(dvp, dir, &dlen) != 0) {\n'
    '\t\t\t\tdir[0] = 0;\n'
    '\t\t\t}\n'
    '\t\t\tvnode_put(dvp);\n'
    '\t\t}\n'
    '\t}\n'
    '\tif (strnstr(p, text, sizeof(p)) == NULL && strnstr(dir, text, sizeof(dir)) == NULL) {\n'
    '\t\treturn;\n'
    '\t}\n'
    '\tlines++;\n'
    '\tprintf("PANDORA PATH: %s %s %s%s%s flags %#x class %d -> %d\\n", name, op, dir, dir[0] ? "/" : "", p,\n'
    '\t    flags, class, error);\n'
    '}\n'
    '#endif /* PANDORA */\n'
    '\n'
    '/*\n * Go through the data-protected atomically controlled open (2)\n')

# openat_dprotected_internal: the body as openat_dprotected_internal1, a wrapper of the old name that logs.
sub(F,
    'static int\nopenat_dprotected_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int class, int dpflags, int fd, int authfd, enum uio_seg segflg, int *retval)\n{\n',
    'static int\nopenat_dprotected_internal1(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int class, int dpflags, int fd, int authfd, enum uio_seg segflg, int *retval)\n{\n')
sub(F,
    '\terror = open1at(vfs_context_current(), &nd, flags, &va,\n\t    NULL, NULL, retval, fd, authfd);\n\n'
    '\treturn error;\n}\n\nint\nopenat_dprotected_np(',
    '\terror = open1at(vfs_context_current(), &nd, flags, &va,\n\t    NULL, NULL, retval, fd, authfd);\n\n'
    '\treturn error;\n}\n\n'
    'static int\n'
    'openat_dprotected_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int class, int dpflags, int fd, int authfd, enum uio_seg segflg, int *retval)\n'
    '{\n'
    '\tint error = openat_dprotected_internal1(ctx, path, flags, mode, class, dpflags, fd, authfd, segflg, retval);\n'
    '#if defined(PANDORA)\n'
    '\tpandora_pathlog_failed(ctx, "open_dprotected", fd, path, segflg, flags, class, error);\n'
    '#endif\n'
    '\treturn error;\n'
    '}\n\n'
    'int\nopenat_dprotected_np(')

# openat_internal: the same.
sub(F,
    'static int\nopenat_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int fd, enum uio_seg segflg, int *retval)\n{\n',
    'static int\nopenat_internal1(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int fd, enum uio_seg segflg, int *retval)\n{\n')
sub(F,
    '}\n\nint\nopen(proc_t p, struct open_args *uap, int32_t *retval)\n',
    '}\n\n'
    'static int\n'
    'openat_internal(vfs_context_t ctx, user_addr_t path, int flags, int mode,\n'
    '    int fd, enum uio_seg segflg, int *retval)\n'
    '{\n'
    '\tint error = openat_internal1(ctx, path, flags, mode, fd, segflg, retval);\n'
    '#if defined(PANDORA)\n'
    '\tpandora_pathlog_failed(ctx, "open", fd, path, segflg, flags, -1, error);\n'
    '#endif\n'
    '\treturn error;\n'
    '}\n\n'
    'int\nopen(proc_t p, struct open_args *uap, int32_t *retval)\n')

# mkdir1at: the same; an existing directory is not logged.
sub(F,
    'static int\nmkdir1at(vfs_context_t ctx, user_addr_t path, struct vnode_attr *vap, int fd,\n'
    '    enum uio_seg segflg)\n{\n',
    'static int\nmkdir1at1(vfs_context_t ctx, user_addr_t path, struct vnode_attr *vap, int fd,\n'
    '    enum uio_seg segflg)\n{\n')
sub(F,
    '/*\n * mkdir_extended: Create a directory; with extended security (ACL).\n',
    'static int\n'
    'mkdir1at(vfs_context_t ctx, user_addr_t path, struct vnode_attr *vap, int fd,\n'
    '    enum uio_seg segflg)\n'
    '{\n'
    '\tint error = mkdir1at1(ctx, path, vap, fd, segflg);\n'
    '#if defined(PANDORA)\n'
    '\tif (error != EEXIST) {\n'
    '\t\tpandora_pathlog_failed(ctx, "mkdir", fd, path, segflg, 0, -1, error);\n'
    '\t}\n'
    '#endif\n'
    '\treturn error;\n'
    '}\n\n'
    '/*\n * mkdir_extended: Create a directory; with extended security (ACL).\n')
