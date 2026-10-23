/*
 * pgputest: the GPU user interface (iokit/Kernel/arm/PandoraAdrenoGpu.cpp) used from a user
 * process the way Turnip's Darwin backend uses it: open PandoraAdrenoGpu with type 0x4147,
 * probe the NTNL tunnel, query, device info, a buffer allocated, mapped for the GPU at the start
 * of the client's window and into this process write-combined, one IB written through that
 * mapping (CP_MEM_WRITE), submitted, waited for, the word read back, then everything released.
 * Every line goes to /dev/console with the prefix "PGPUTEST:", so it lands in the kernel log.
 *
 * No SDK: the few libSystem and IOKit functions are declared here and linked against the
 * stubs in this directory (libSystem.tbd, IOKit.tbd).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef const void *CFDictionaryRef;
extern mach_port_t mach_task_self_;
int open(const char *path, int flags, ...);
long write(int fd, const void *buf, unsigned long n);
int close(int fd);
unsigned int sleep(unsigned int s);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
CFDictionaryRef IOServiceMatching(const char *name);
mach_port_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
kern_return_t IOServiceOpen(mach_port_t service, mach_port_t task, uint32_t type, mach_port_t *conn);
kern_return_t IOServiceClose(mach_port_t conn);
kern_return_t IOConnectCallStructMethod(mach_port_t conn, uint32_t selector, const void *in, size_t in_n,
    void *out, size_t *out_n);
kern_return_t IOConnectMapMemory64(mach_port_t conn, uint32_t type, mach_port_t task, uint64_t *addr,
    uint64_t *size, uint32_t options);
kern_return_t IOConnectUnmapMemory64(mach_port_t conn, uint32_t type, mach_port_t task, uint64_t addr);

#define O_WRONLY                1
#define kIOMapAnywhere          0x1u
#define kIOMapWriteCombineCache 0x400u
#define TUNNEL                  0x4e544e4cu

static int con = -1;
static unsigned failures;

static void
say(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(buf) - 2) {
		n = (int)sizeof(buf) - 2;
	}
	buf[n++] = '\n';
	(void)write(con, buf, (unsigned long)n);
}

static void
check(const char *what, kern_return_t kr)
{
	say("PGPUTEST: %s -> 0x%x%s", what, kr, kr ? " FAIL" : "");
	failures += kr != 0;
}

static mach_port_t conn;

/* Direct selector, as Turnip's direct mode (TU_DARWIN_SERVICE) calls it. */
static kern_return_t
call(uint32_t m, const void *in, size_t in_n, void *out, size_t out_n)
{
	size_t got = out_n;
	kern_return_t kr = IOConnectCallStructMethod(conn, m, in, in_n, out_n ? out : NULL, out_n ? &got : NULL);
	if (!kr && got != out_n) {
		say("PGPUTEST: method %u returned %zu bytes, want %zu", m, got, out_n);
		return -1;
	}
	return kr;
}

/* PKT7 header (a6xx PM4_PARITY), as pandora_gpu.c builds it. */
static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	       (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (parity(cnt) << 15) | ((op & 0x7f) << 16) | (parity(op) << 23);
}

int
main(void)
{
	con = open("/dev/console", O_WRONLY);
	sleep(5);
	say("PGPUTEST: start");
	mach_port_t svc = IOServiceGetMatchingService(0, IOServiceMatching("PandoraAdrenoGpu"));
	say("PGPUTEST: service %#x", svc);
	if (!svc) {
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self_, 0x4147, &conn);
	check("open type 0x4147", kr);
	if (kr) {
		return 1;
	}

	/* The tunnel probe Turnip sends on selector 0, then Query through the tunnel. */
	uint32_t probe[2] = { TUNNEL, 0xffffffffu };
	check("tunnel probe", call(0, probe, sizeof(probe), NULL, 0));
	struct { uint32_t abi, in_bits, out_bits, flags; uint64_t max_bo; } params;
	uint32_t tq[2] = { TUNNEL, 1 };
	check("query (tunnel)", call(0, tq, sizeof(tq), &params, sizeof(params)));
	say("PGPUTEST: abi %u input bits %u output bits %u flags %u max bo %#llx", params.abi, params.in_bits,
	    params.out_bits, params.flags, (unsigned long long)params.max_bo);

	struct {
		uint32_t chip, gmem; uint64_t gmem_base, trap, va_start, va_size;
		uint32_t hbb, swizzle, macrotile, ubwc, flags, reserved;
	} info;
	check("device info", call(15, NULL, 0, &info, sizeof(info)));
	say("PGPUTEST: chip %08x gmem %u at %#llx window %#llx+%#llx ubwc %u hbb %u flags %u", info.chip, info.gmem,
	    (unsigned long long)info.gmem_base, (unsigned long long)info.va_start,
	    (unsigned long long)info.va_size, info.ubwc, info.hbb, info.flags);

	struct { uint64_t bytes; uint32_t align_log2, contiguous; } alloc = { 0x10000, 12, 0 };
	uint64_t handle = 0;
	check("bo alloc 64 KiB", call(2, &alloc, sizeof(alloc), &handle, sizeof(handle)));
	struct { uint64_t handle, iova; uint32_t perm, writeback, share, reserved; } map =
	    { handle, info.va_start, 3, 0, 2, 0 };
	uint64_t iova = 0;
	check("bo map at window start", call(3, &map, sizeof(map), &iova, sizeof(iova)));
	struct { uint32_t token, reserved; uint64_t bytes; } view;
	check("bo cpu map", call(13, &handle, sizeof(handle), &view, sizeof(view)));
	uint64_t addr = 0, size = 0;
	check("IOConnectMapMemory64 write-combined", IOConnectMapMemory64(conn, view.token, mach_task_self_, &addr,
	    &size, kIOMapAnywhere | kIOMapWriteCombineCache));
	say("PGPUTEST: buffer %#llx token %u mapped at %#llx size %#llx", (unsigned long long)handle, view.token,
	    (unsigned long long)addr, (unsigned long long)size);

	if (addr) {
		volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
		const uint64_t target = iova + 0x8000;
		p[0] = pkt7(0x3d, 3);           /* CP_MEM_WRITE */
		p[1] = (uint32_t)target;
		p[2] = (uint32_t)(target >> 32);
		p[3] = 0x15151515;
		__asm__ volatile ("dsb sy" ::: "memory");
		struct {
			uint32_t count, reserved; uint64_t queue, wait_fence, wait_timeout;
			struct { uint64_t handle, iova, bytes; } b[16];
		} sub = { 1, 0, 0, 0, 0, { { handle, iova, 16 } } };
		uint64_t fence = 0;
		check("submit", call(5, &sub, sizeof(sub), &fence, sizeof(fence)));
		struct { uint64_t fence, timeout; } w = { fence, 1000000000ull };
		check("wait fence", call(6, &w, sizeof(w), NULL, 0));
		uint32_t got = p[0x8000 / 4];
		say("PGPUTEST: fence %llu, word written by the GPU %08x (want 15151515)%s", (unsigned long long)fence,
		    got, got == 0x15151515 ? "" : " FAIL");
		failures += got != 0x15151515;
	}

	uint64_t ticks = 0;
	check("timestamp", call(7, NULL, 0, &ticks, sizeof(ticks)));
	say("PGPUTEST: timestamp %llu", (unsigned long long)ticks);

	struct { uint64_t handle; uint32_t bytes, reserved; char text[64]; } meta = { handle, 7, 0, "pgputst" }, back;
	check("set metadata", call(9, &meta, sizeof(meta), NULL, 0));
	meta.bytes = 64;
	check("get metadata", call(10, &meta, sizeof(meta), &back, sizeof(back)));
	say("PGPUTEST: metadata %u bytes '%.7s'", back.bytes, back.text);

	if (addr) {
		check("IOConnectUnmapMemory64", IOConnectUnmapMemory64(conn, view.token, mach_task_self_, addr));
	}
	struct { uint64_t handle; uint32_t token, reserved; } unview = { handle, view.token, 0 };
	check("bo cpu unmap", call(14, &unview, sizeof(unview), NULL, 0));
	check("bo free", call(4, &handle, sizeof(handle), NULL, 0));
	check("close", IOServiceClose(conn));
	say("PGPUTEST: %s (%u failures)", failures ? "FAIL" : "PASS", failures);
	close(con);
	return failures != 0;
}
