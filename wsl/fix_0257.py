W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0257: limits from the body temperature ---------------------------------------------------------------------
# usb8: all 14 thermistors read right, the body temperature (mi_thermald's VIRTUAL-SENSOR-FORMULA) went 36.9 -> 40.6
# degrees in 50 s while the cores still reached 105 C. pandora_therm=2 now computes it every second and applies
# mi_thermald's thermal-normal tables ([SS-CPU0], [SS-CPU6], [MONITOR-GPU]; user's OK 2026-10-28): a kHz cap per CPU
# cluster (pandora_cpu_body_cap, held by the governor as the junction cap is) and a GPU level cap
# (pandora_gpu_body_top, held by the DCVS as pandora_gpu_thermal_top is). The thread starts at 2 s instead of 10 s,
# so the limits hold from the gauge's first reading (about 8.5 s).
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched (limits from the body temperature)')
