/*
 * Test of wsl/pandora_sha256_ce.c (the kernel's SHA-256 with ARMv8 SHA-2 instructions) as a
 * freestanding aarch64 Linux program: prints the digests of fixed inputs, which
 * tools/sha256ce_test.sh compares with Python's hashlib. No libc: raw write/exit syscalls.
 */
#include <stddef.h>
#include <stdint.h>

void ccsha256_pandora_ce_compress(void *state, size_t nblocks, const void *in);

const uint32_t ccsha256_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static long
sys(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static void
out(const char *s, size_t n)
{
	sys(64, 1, (long)s, (long)n);
}

static uint8_t big[1 << 20];
static uint8_t tail[128];

/* SHA-256 of DATA (LEN bytes) with the compress function under test; padding done here. */
static void
sha256(const uint8_t *data, size_t len, uint8_t digest[32])
{
	uint32_t st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
		           0x5be0cd19 };
	size_t full = len / 64;
	if (full) {
		ccsha256_pandora_ce_compress(st, full, data);
	}
	size_t rest = len - full * 64, n = rest < 56 ? 64 : 128;
	for (size_t i = 0; i < sizeof(tail); i++) {
		tail[i] = 0;
	}
	for (size_t i = 0; i < rest; i++) {
		tail[i] = data[full * 64 + i];
	}
	tail[rest] = 0x80;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		tail[n - 1 - i] = (uint8_t)(bits >> (8 * i));
	}
	ccsha256_pandora_ce_compress(st, n / 64, tail);
	for (int i = 0; i < 8; i++) {
		digest[4 * i] = (uint8_t)(st[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(st[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(st[i] >> 8);
		digest[4 * i + 3] = (uint8_t)st[i];
	}
}

static void
show(const char *name, const uint8_t *data, size_t len)
{
	uint8_t d[32];
	char line[100];
	size_t k = 0;
	sha256(data, len, d);
	while (*name) {
		line[k++] = *name++;
	}
	line[k++] = ' ';
	for (int i = 0; i < 32; i++) {
		line[k++] = "0123456789abcdef"[d[i] >> 4];
		line[k++] = "0123456789abcdef"[d[i] & 15];
	}
	line[k++] = '\n';
	out(line, k);
}

void
_start(void)
{
	static const char abc[] = "abc";
	static const char two[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	uint32_t x = 1;
	for (size_t i = 0; i < sizeof(big); i++) {     /* xorshift32, the same in the checker */
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		big[i] = (uint8_t)x;
	}
	show("empty", (const uint8_t *)abc, 0);
	show("abc", (const uint8_t *)abc, 3);
	show("448bits", (const uint8_t *)two, 56);
	for (size_t n = 1; n <= 130; n++) {             /* every padding case */
		char name[8] = { 'n', (char)('0' + n / 100), (char)('0' + n / 10 % 10), (char)('0' + n % 10), 0 };
		show(name, big, n);
	}
	show("1MiB", big, sizeof(big));
	show("16KiB", big + 4096, 16384);
	show("4KiB", big + 8192, 4096);
	sys(93, 0, 0, 0);
	for (;;) {
	}
}
