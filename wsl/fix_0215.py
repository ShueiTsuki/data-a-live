W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Cores idle in the DT's "ret" state (PSCI CPU_SUSPEND 0x4) when their next timer is at least the DT's
# min-residency away, in place of WFI where Apple's chips use arm64_retention_wfi; per-CPU counts every 10 s.
copy('pandora_cpu_idle.c', 'osfmk/arm/pandora_cpu_idle.c')
copy('pandora_perfctl.c', 'osfmk/arm/pandora_perfctl.c')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
edit('osfmk/arm64/cpu.c', '''extern boolean_t        idle_enable;
''', '''extern boolean_t        idle_enable;
#if defined(PANDORA)
extern void pandora_idle_wfi(unsigned cpu, uint64_t pop);   /* osfmk/arm/pandora_cpu_idle.c (0215) */
#endif
''')
edit('osfmk/arm64/cpu.c', '''		__builtin_arm_dsb(DSB_SY);
#if HAS_RETENTION_STATE
		arm64_retention_wfi();
#else''', '''		__builtin_arm_dsb(DSB_SY);
#if defined(PANDORA)
		/* 0215: the DT's core retention state when the next timer is far enough */
		pandora_idle_wfi(cpu_data_ptr->cpu_number, cpu_data_ptr->rtcPop);
#elif HAS_RETENTION_STATE
		arm64_retention_wfi();
#else''')

# The display controller as the ABL left it, read only, once at the framebuffer's start (bsd/dev/pandora_dpu.c).
copy('pandora_dpu.c', 'bsd/dev/pandora_dpu.c')
edit('bsd/conf/files', 'bsd/dev/pandora_cpu.c\t\t\tstandard\n',
     'bsd/dev/pandora_cpu.c\t\t\tstandard\nbsd/dev/pandora_dpu.c\t\t\tstandard\n')
S = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(S, '''extern "C" void pandora_fbdump_commit(uint32_t len);''', '''extern "C" void pandora_fbdump_commit(uint32_t len);
extern "C" void pandora_dpu_probe(uint64_t fb);                 /* bsd/dev/pandora_dpu.c (0215) */''')
edit(S, '''		IOLog("SWEET FB: no boot framebuffer\\n");
		return false;
	}
	int native = 0;''', '''		IOLog("SWEET FB: no boot framebuffer\\n");
		return false;
	}
#if defined(PANDORA)
	pandora_dpu_probe(fPhys);
#endif
	int native = 0;''')
print('patched')
