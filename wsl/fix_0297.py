W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0297: the file installer removes files ("! - PATH" in files.txt) -------------------------------------------
# aud24-26 installed a listening-test player (/Applications/Camera.app/PP, an earlier copy as PandoraPlayer) and its
# music (/private/var/mobile/Media/audplay.m4a). The user: "别忘了之后换回去". The installer can now remove them,
# with unlink1 (bsd/vfs/vfs_syscalls.c) on kernel-space paths. pandora_adsp.c: only the 0276 comment, which said the
# stream volume control had no calibration (it has the ACDB's Volume table; tools/spk_graph.py now takes Level_0).
copy('pandora_files.c', 'bsd/dev/pandora_files.c')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0297 installer removes)')
