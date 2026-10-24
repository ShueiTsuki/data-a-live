W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Boot time, second part (boot2: launchd waited 33 s while cs_native4k_prepare hashed the 3.2 GB dyld
# shared cache twice with the portable C SHA-256, 200 MB/s for both passes). SHA256_Init/Update/Final
# (code signing's page hashes, among others) now use the ARMv8 SHA-2 instructions, as Apple's
# corecrypto does (ccsha256_vng_arm); checked against hashlib under qemu-aarch64 by
# tools/sha256ce_test.sh. The early PRNG keeps its own direct use of ccsha256_ltc_di.
copy('pandora_sha256_ce.c', 'osfmk/corecrypto/ccsha256_pandora_ce.c')
edit('osfmk/conf/files.arm64', 'osfmk/arm/pandora_perfctl.c\tstandard\n',
     'osfmk/arm/pandora_perfctl.c\tstandard\nosfmk/corecrypto/ccsha256_pandora_ce.c\tstandard\n')
S = 'libkern/crypto/corecrypto_sha2.c'
edit(S, '''#include <corecrypto/ccsha2.h>
#define SHA256_DI()                     (&ccsha256_ltc_di)''', '''#include <corecrypto/ccsha2.h>
#if defined(PANDORA)
/* 0211: the ARMv8 SHA-2 instructions (osfmk/corecrypto/ccsha256_pandora_ce.c), as corecrypto's vng_arm */
extern const struct ccdigest_info ccsha256_pandora_ce_di;
#define SHA256_DI()                     (&ccsha256_pandora_ce_di)
#else
#define SHA256_DI()                     (&ccsha256_ltc_di)
#endif''')
print('patched')
