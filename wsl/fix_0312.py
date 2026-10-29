W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0312: the governor with fewer CPUs started --------------------------------------------------------------------------
# shot9 (cpus=6, the first cluster only, to see whether Settings' words still change with one cluster): the governor's
# idle-time and clock readings asked cpu_to_processor for cpu 6 and 7 and the kernel stopped at cpu_common.c:82
# (cpu <= ml_get_max_cpu_number()). Both now answer 0 for a CPU that was not started.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('pandora_cpu_idle.c', 'osfmk/arm/pandora_cpu_idle.c')
print('patched (0312 cpus not started)')
