W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Boot time, first part (boot1: kernel lines time-stamped by 0208/0209).
#
# 1. bsd_init's IOKitInitializeTime waits up to 30 s for the "IORTC" resource, which an iPhone's PMU RTC
#    driver publishes; nothing publishes it here, so every boot sat 30.0 s silent between "using N buffer
#    headers" (0.155 s) and the next line (30.156 s). The platform expert publishes it now: the calendar
#    starts at once from getGMTTimeOfDay, 0 (1970) as before, until an RTC driver reads the PMIC's RTC.
S = 'iokit/Kernel/arm/SweetPlatformExpert.cpp'
edit(S, '''	registerService();
	IOLog("SWEET: platform expert registered\\n");''', '''	registerService();
#if defined(PANDORA)
	/* 0210: what an iPhone's RTC driver publishes; IOKitInitializeTime waits up to 30 s for it. */
	publishResource("IORTC", this);
#endif
	IOLog("SWEET: platform expert registered\\n");''')

# 2. Then launchd took 33 s to say hello: the dyld shared cache (3.2 GB, signed with 16 KiB page hashes)
#    is read whole and hashed twice by cs_native4k_prepare (the 16 KiB check, then 4 KiB hashes for this
#    4 KiB-page kernel) on one CPU; storage was busy 3 s of it (PANDORA IO). Time the reads and the
#    hashing of every big file to see which to speed up.
S = 'bsd/kern/ubc_subr.c'
edit(S, '''	vm_size_t bufsize = 1024 * 1024;        /* a multiple of 16384 */
	uint64_t limit, chunk;''', '''	vm_size_t bufsize = 1024 * 1024;        /* a multiple of 16384 */
	uint64_t limit, chunk;
	uint64_t t_read = 0, t_hash = 0, t_mark;   /* 0210: where boot time goes */''')
edit(S, '''		error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)chunk_length, position,
		    UIO_SYSSPACE, 0, vfs_context_ucred(vfs_context_current()),
		    &resid, current_proc());''', '''		t_mark = mach_absolute_time();
		error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)chunk_length, position,
		    UIO_SYSSPACE, 0, vfs_context_ucred(vfs_context_current()),
		    &resid, current_proc());
		t_read += mach_absolute_time() - t_mark;
		t_mark = mach_absolute_time();''')
edit(S, '''				memcpy((void *)(storage + index * md->cs_size), digest, md->cs_size);
			}
		}
	}''', '''				memcpy((void *)(storage + index * md->cs_size), digest, md->cs_size);
			}
		}
		t_hash += mach_absolute_time() - t_mark;
	}''')
edit(S, '''	storage = 0;
	os_atomic_inc(&cs_native4k_ready_blobs, relaxed);''', '''	storage = 0;
	os_atomic_inc(&cs_native4k_ready_blobs, relaxed);
	if (limit >= (32ull << 20)) {
		extern int cpu_number(void);
		uint64_t rn, hn;
		absolutetime_to_nanoseconds(t_read, &rn);
		absolutetime_to_nanoseconds(t_hash, &hn);
		const char *name = vnode_getname_printable(vp);
		printf("PANDORA CS4K: %s %llu MiB (cpu %d at the end): read %llu ms, hash %llu ms, %llu MB/s hashed (both passes)\\n",
		    name, limit >> 20, cpu_number(), rn / 1000000, hn / 1000000, hn ? (2 * limit * 1000) / hn : 0);
		vnode_putname_printable(name);
	}''')
print('patched')
