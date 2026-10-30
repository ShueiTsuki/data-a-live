W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0332: 15 GPU clients ----------------------------------------------------------------------------------------
# Towards every process having Metal: the 36-bit GPU context has room for 15 windows of 4 GiB after the kernel's own
# 4 GiB, where only 4 clients were allowed (SpringBoard, PosterBoard, the posters, chronod and Photos were refused a
# device). The table pool goes from 1024 to 2048 pages, the watch line shows its use, and a refused open is logged.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
