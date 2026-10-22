def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Disk statistics: where does a slow boot (lsd reseeding the LaunchServices database)
# spend its time? Every pandora_iostats seconds (default 30, 0 = off) the log thread
# prints what the UFS driver did in that interval: commands, bytes, device time (doorbell
# to completion), time in ufs_scsi (device + cache maintenance) and time spent waiting
# for the driver's mutex. Only counters; no I/O path changes.
patch('bsd/dev/sweet_ufs.c', [
    ("""static LCK_GRP_DECLARE(sweet_ufs_grp, "sweet_ufs");""",
     """#if defined(PANDORA)
/* Disk statistics (pandora_iostats); absolute-time units, updated under sweet_ufs_lock. */
static struct pandora_io {
	uint64_t rd_cmds, rd_bytes, wr_cmds, wr_bytes, other_cmds;
	uint64_t dev_time, scsi_time, lock_wait, max_dev, bufs;
} pio;
#endif

static LCK_GRP_DECLARE(sweet_ufs_grp, "sweet_ufs");"""),
    ("""	__builtin_arm_dsb(0xf);

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline;""",
     """	__builtin_arm_dsb(0xf);

#if defined(PANDORA)
	uint64_t pio_t0 = mach_absolute_time();
#endif
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline;"""),
    ("""	__builtin_arm_dsb(0xf);
	ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */
""",
     """	__builtin_arm_dsb(0xf);
#if defined(PANDORA)
	uint64_t pio_dev = mach_absolute_time() - pio_t0;
	pio.dev_time += pio_dev;
	if (pio_dev > pio.max_dev) {
		pio.max_dev = pio_dev;
	}
	if (len && write) {
		pio.wr_cmds++;
		pio.wr_bytes += len;
	} else if (len) {
		pio.rd_cmds++;
		pio.rd_bytes += len;
	} else {
		pio.other_cmds++;
	}
#endif
	ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */
"""),
    ("""	lck_mtx_lock(&sweet_ufs_lock);
	for (uint32_t done = 0; done < count;) {""",
     """#if defined(PANDORA)
	uint64_t pio_w = mach_absolute_time();
#endif
	lck_mtx_lock(&sweet_ufs_lock);
#if defined(PANDORA)
	uint64_t pio_in = mach_absolute_time();
	pio.lock_wait += pio_in - pio_w;
	pio.bufs++;
#endif
	for (uint32_t done = 0; done < count;) {"""),
    ("""		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&sweet_ufs_lock);""",
     """		done += want;
		buf_setresid(bp, count - done);
	}
#if defined(PANDORA)
	pio.scsi_time += mach_absolute_time() - pio_in;
#endif
	lck_mtx_unlock(&sweet_ufs_lock);"""),
    ("""static void
pl_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t secs = 3, reboot_after = 0;
	PE_parse_boot_argn("pandora_logsecs", &secs, sizeof(secs));""",
     """static uint64_t
pio_ms(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_MSEC;
}

/* One line per interval: the driver's work since the last line (sweet_ufs_lock held). */
static void
pio_print(uint64_t interval)
{
	static struct pandora_io last;
	struct pandora_io d = {
		.rd_cmds = pio.rd_cmds - last.rd_cmds, .rd_bytes = pio.rd_bytes - last.rd_bytes,
		.wr_cmds = pio.wr_cmds - last.wr_cmds, .wr_bytes = pio.wr_bytes - last.wr_bytes,
		.other_cmds = pio.other_cmds - last.other_cmds, .bufs = pio.bufs - last.bufs,
		.dev_time = pio.dev_time - last.dev_time, .scsi_time = pio.scsi_time - last.scsi_time,
		.lock_wait = pio.lock_wait - last.lock_wait,
	};
	last = pio;
	uint64_t ms = pio_ms(interval), dev = pio_ms(d.dev_time);
	printf("PANDORA IO: %llu ms: bufs %llu, read %llu cmds %llu KiB, write %llu cmds %llu KiB, "
	    "other %llu; device %llu ms (%llu%%), max %llu us; driver %llu ms; lock wait %llu ms\\n",
	    ms, d.bufs, d.rd_cmds, d.rd_bytes >> 10, d.wr_cmds, d.wr_bytes >> 10, d.other_cmds,
	    dev, ms ? dev * 100 / ms : 0, pio_ms(pio.max_dev * 1000), pio_ms(d.scsi_time),
	    pio_ms(d.lock_wait));
	pio.max_dev = 0;
}

static void
pl_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t secs = 3, reboot_after = 0, iostats = 30;
	PE_parse_boot_argn("pandora_logsecs", &secs, sizeof(secs));
	PE_parse_boot_argn("pandora_iostats", &iostats, sizeof(iostats));"""),
    ("""	printf("PANDORA LOG: console log to the pinned log file every %u s%s%u\\n", secs,
	    reboot_after ? ", reset after s " : "", reboot_after);
	for (;;) {
		(void)tsleep(&pl_chan, PZERO, "pandoralog", hz * (int)secs);
		lck_mtx_lock(&sweet_ufs_lock);
		int error = pl_flush();""",
     """	printf("PANDORA LOG: console log to the pinned log file every %u s%s%u\\n", secs,
	    reboot_after ? ", reset after s " : "", reboot_after);
	uint64_t io_every = 0, io_last = start;
	nanoseconds_to_absolutetime((uint64_t)iostats * NSEC_PER_SEC, &io_every);
	for (;;) {
		(void)tsleep(&pl_chan, PZERO, "pandoralog", hz * (int)secs);
		lck_mtx_lock(&sweet_ufs_lock);
		uint64_t now = mach_absolute_time();
		if (io_every && now - io_last >= io_every) {
			pio_print(now - io_last);
			io_last = now;
		}
		int error = pl_flush();"""),
])
print('patched')
