W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# Thermal control from the phone's DT thermal zones (bsd/dev/pandora_cpu.c cpu_thermal): CPU level caps
# (105 C, hysteresis 5), hot cores out of the scheduler through Apple's performance-controller interface
# (114 C, hysteresis 6), the lowest level at 118 C, the GPU's level cap (95 C); boot-arg
# pandora_thermal_test=N lowers every threshold for tests.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('pandora_cpu_idle.c', 'osfmk/arm/pandora_cpu_idle.c')
print('patched')
