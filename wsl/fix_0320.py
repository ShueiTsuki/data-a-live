W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# ---- 0320: two clusters for the scheduler (AMP, Apple's Edge scheduler) and P-spill placement ----------------------
# The board config described the eight Oryon cores as one cluster (ios-sweet's MAX_CPU_CLUSTERS 1, which also
# undefines __ARM_AMP__), so the scheduler treated them alike and took idle CPUs from the lowest number up: the six
# performance cores ran the UI at about 1.7-2 GHz while the two prime cores stayed near 5% busy at 768 MHz (post14).
# With two clusters ml_parse_cpu_topology takes each core's cluster from its device tree "cluster-type" ('E' for
# cores 0-5, 'P' for 6-7: mkadt_pandora.py --amp; without it every core is one SMP cluster as before), and the
# macOS-target kernel's AMP configuration is the Edge scheduler (osfmk/arm64/proc_reg.h). Placement is the
# performance controller's: osfmk/arm/pandora_perfctl.c recommends P-spill for every thread group.
sub('pexpert/pexpert/arm64/board_config.h',
    '#include <pexpert/arm64/SWEET.h>\n\n'
    '#define MAX_L2_CLINE                   6\n'
    '#define MAX_CPUS                       8\n'
    '#define MAX_CPU_CLUSTERS               1\n',
    '#include <pexpert/arm64/SWEET.h>\n\n'
    '#define MAX_L2_CLINE                   6\n'
    '#define MAX_CPUS                       8\n'
    '#if defined(PANDORA)\n'
    '/* Six Oryon performance cores (the E cluster to the scheduler) and two prime cores (P): patch 0320. */\n'
    '#define MAX_CPU_CLUSTERS               2\n'
    '#define __ARM_AMP__                    1\n'
    '#else\n'
    '#define MAX_CPU_CLUSTERS               1\n'
    '#endif\n')
copy('pandora_perfctl.c', 'osfmk/arm/pandora_perfctl.c')
