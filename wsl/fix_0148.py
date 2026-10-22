import subprocess

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
UNIXNU = '/mnt/d/DeepSeekHarness/claude/unixnu_review/src/kits/NativePrelink/patches/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# UniXNU NativePrelink sparse header index for kalloc_type_view_parse (BSD-2-Clause-Patent,
# UniXNU contributors); applies to xnu-8796 with a one-line offset.
diff = open(UNIXNU + 'xnu-10063.141.1-sparse-index.patch', 'rb').read().replace(b'\r', b'')
subprocess.run(['patch', '-p1', '--no-backup-if-mismatch'], input=diff, check=True)
patch('osfmk/kern/kalloc.c', [
    ("__attribute__((used)) const uint32_t kalloc_sparse_prelink_index_version = 1;\n",
     "/* From UniXNU NativePrelink (BSD-2-Clause-Patent): PANDORA prelinks a few original kexts\n"
     " * (tools/mkprelink.py), so __PRELINK_TEXT holds sparse headers listed in __macho_list. */\n"
     "__attribute__((used)) const uint32_t kalloc_sparse_prelink_index_version = 1;\n"),
])

open('iokit/Kernel/arm/PandoraPMGRNub.cpp', 'w').write(open(W + 'PandoraPMGRNub.cpp').read().replace('\r', ''))
patch('iokit/conf/files.arm64', [
    ("iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n",
     "iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n"
     "iokit/Kernel/arm/PandoraPMGRNub.cpp                              optional iokitcpp\n"),
])
patch('bsd/kern/bsd_init.c', [
    ("""#if defined(ARM64_BOARD_CONFIG_SWEET)
	/* AMFI's MAC policy is built in on SWEET (bsd/kern/sweet_amfi.c). */""",
     """#if defined(PANDORA)
	/* ApplePMGRNub for a prelinked original AMFI (iokit/Kernel/arm/PandoraPMGRNub.cpp). */
	extern void pandora_pmgr_init(void);
	pandora_pmgr_init();
#endif

#if defined(ARM64_BOARD_CONFIG_SWEET)
	/* AMFI's MAC policy is built in on SWEET (bsd/kern/sweet_amfi.c). */"""),
])
print('patched')
