W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0224: GPU SMMU TLB maintenance as Linux's arm-smmu; the home screen swiped by itself for unattended tests -------
# r2k's call stacks: 29% of backboardd's kernel samples were register reads under ArmSmmu4KInvalidateAllOwnedContext,
# which every GPU map and unmap called (the bank's registers read back before and after a TLBIALL). A map now
# invalidates nothing (it only fills empty entries; the SMMU caches no faulting entry), an unmap invalidates its pages
# by VA and ASID and waits once for TLBSYNC, as arm-smmu with io-pgtable-arm does; written table entries are cleaned a
# run at a time. pandora_autoswipe=SECONDS pages the home screen by itself, so the profiler sees the screen moving
# while nobody holds the phone.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
copy('pandora_autoswipe.c', 'bsd/dev/pandora_autoswipe.c')
edit('bsd/conf/files', '''bsd/dev/pandora_cpu.c\t\t\tstandard
''', '''bsd/dev/pandora_cpu.c\t\t\tstandard
bsd/dev/pandora_autoswipe.c\t\tstandard
''')
print('patched (SMMU TLB maintenance, autoswipe)')
