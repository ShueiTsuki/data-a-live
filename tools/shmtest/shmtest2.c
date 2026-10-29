/*
 * shmtest2: more of what an app and the render server rely on, in one run (after tools/shmtest/shmtest.c, whose checks
 * of shared and copied memory inside one task all passed: shm1).
 *   zero:   fresh memory (vm_allocate, purgeable or not, 16 KiB to 1 MiB) reads all zero, 2000 times
 *   cross:  a child (fork) makes buffers as QuartzCore does (vm_allocate PURGABLE, mach_make_memory_entry_64 with
 *           MAP_MEM_VM_SHARE | MAP_MEM_USE_DATA_ADDR) and sends the entries; the parent maps each; then rounds where the
 *           child writes a value and the parent checks its mapping, with volatile/nonvolatile and new buffers between
 *   oolx:   the child sends bytes out of line (virtual copy) and writes its buffer right after the send; the parent must
 *           receive the bytes as sent
 *   fork:   after fork each side's writes stay its own
 *   order:  one thread writes 16 words then a sequence number (release); another reads the number (acquire) and then the
 *           words, which must be at least that number; 4 pairs, 5 million rounds each
 *   time:   mach_timebase_info, and mach_absolute_time against CLOCK_MONOTONIC over 2 s
 * Each part prints one "SHMTEST2:" summary line, and the first few failures. Runs in place of bluetoothd.
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_MEM_VM_SHARE
#define MAP_MEM_VM_SHARE        0x400000
#endif
#ifndef MAP_MEM_USE_DATA_ADDR
#define MAP_MEM_USE_DATA_ADDR   0x100000
#endif

static int con = -1;

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
	(void)write(con, buf, (size_t)n);
}

/* First byte of SIZE at P that is not V, or -1. */
static long
differs(const volatile unsigned char *p, size_t size, unsigned char v)
{
	for (size_t i = 0; i < size; i++) {
		if (p[i] != v) {
			return (long)i;
		}
	}
	return -1;
}

/* ---- zero ---------------------------------------------------------------------------------------------------------- */
static void
test_zero(void)
{
	int bad = 0;
	for (int r = 0; r < 2000; r++) {
		const vm_size_t size = (vm_size_t)(16 * 1024) << (r % 7);
		const int flags = VM_FLAGS_ANYWHERE | ((r & 1) ? VM_FLAGS_PURGABLE | VM_MAKE_TAG(0x33) : 0);
		vm_address_t a = 0;
		if (vm_allocate(mach_task_self(), &a, size, flags) != KERN_SUCCESS) {
			bad++;
			continue;
		}
		const long at = differs((const volatile unsigned char *)a, size, 0);
		if (at >= 0) {
			if (bad < 5) {
				say("SHMTEST2: zero: round %d, %lu KiB%s: byte %ld is 0x%02x FAIL", r, (unsigned long)(size >> 10),
				    (r & 1) ? " purgeable" : "", at, ((volatile unsigned char *)a)[at]);
			}
			bad++;
		}
		memset((void *)a, 0x5a + r, size);
		vm_deallocate(mach_task_self(), a, size);
	}
	say("SHMTEST2: zero: 2000 fresh buffers, %d not all zero", bad);
}

/* ---- cross, oolx: parent and child ---------------------------------------------------------------------------------- */
enum { M_HELLO = 1, M_ENTRY, M_WROTE, M_OK, M_OOL, M_END };
#define BUFSIZE (64 * 1024)
#define ROUNDS 300

typedef struct {
	mach_msg_header_t h;
	mach_msg_body_t b;
	mach_msg_port_descriptor_t p;   /* M_HELLO: the child's port; M_ENTRY: a memory entry */
	mach_msg_ool_descriptor_t o;    /* M_OOL */
	uint32_t kind, value, round;
} msg_t;

typedef struct {
	msg_t m;
	mach_msg_trailer_t t;
} rmsg_t;

static kern_return_t
send(mach_port_t to, uint32_t kind, uint32_t value, uint32_t round, mach_port_t port, mach_msg_type_name_t disp,
    void *ool, vm_size_t oolsize)
{
	msg_t m;
	memset(&m, 0, sizeof m);
	m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = to;
	m.b.msgh_descriptor_count = 2;
	m.p.name = port;
	m.p.disposition = port ? disp : MACH_MSG_TYPE_COPY_SEND;
	m.p.type = MACH_MSG_PORT_DESCRIPTOR;
	m.o.address = ool;
	m.o.size = (mach_msg_size_t)oolsize;
	m.o.deallocate = FALSE;
	m.o.copy = MACH_MSG_VIRTUAL_COPY;
	m.o.type = MACH_MSG_OOL_DESCRIPTOR;
	m.kind = kind;
	m.value = value;
	m.round = round;
	return mach_msg(&m.h, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

static kern_return_t
receive(mach_port_t on, rmsg_t *r)
{
	memset(r, 0, sizeof *r);
	return mach_msg(&r->m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof *r, on, 10000, MACH_PORT_NULL);
}

/* The child: QuartzCore's side. */
static void
child(void)
{
	mach_port_t *reg = NULL, parent = MACH_PORT_NULL, mine = MACH_PORT_NULL;
	mach_msg_type_number_t n = 0;
	if (mach_ports_lookup(mach_task_self(), &reg, &n) != KERN_SUCCESS || n < 1 || reg[0] == MACH_PORT_NULL) {
		_exit(2);
	}
	parent = reg[0];
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mine);
	send(parent, M_HELLO, 0, 0, mine, MACH_MSG_TYPE_MAKE_SEND, NULL, 0);
	rmsg_t r;
	vm_address_t a = 0;
	for (uint32_t round = 0; round < ROUNDS; round++) {
		if (a == 0 || round % 10 == 0) {    /* a new buffer every 10 rounds, as buffers come and go */
			if (a) {
				vm_deallocate(mach_task_self(), a, BUFSIZE);
			}
			a = 0;
			vm_allocate(mach_task_self(), &a, BUFSIZE, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE | VM_MAKE_TAG(0x33));
			memory_object_size_t es = BUFSIZE;
			mach_port_t entry = MACH_PORT_NULL;
			mach_make_memory_entry_64(mach_task_self(), &es, a, VM_PROT_READ | MAP_MEM_VM_SHARE | MAP_MEM_USE_DATA_ADDR,
			    &entry, MACH_PORT_NULL);
			memset((void *)a, 0x10 + round % 200, BUFSIZE);
			send(parent, M_ENTRY, 0x10 + round % 200, round, entry, MACH_MSG_TYPE_MOVE_SEND, NULL, 0);
			if (receive(mine, &r) != KERN_SUCCESS) {
				_exit(3);
			}
		}
		if (round % 3 == 1) {               /* back from the pool, as QuartzCore reuses a buffer */
			int s = VM_PURGABLE_VOLATILE;
			vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &s);
			s = VM_PURGABLE_NONVOLATILE;
			vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &s);
		}
		const uint32_t v = 0x20 + (round * 7) % 200;
		memset((void *)a, (int)v, BUFSIZE);
		send(parent, M_WROTE, v, round, MACH_PORT_NULL, 0, NULL, 0);
		if (receive(mine, &r) != KERN_SUCCESS) {
			_exit(4);
		}
	}
	/* oolx: out of line, the buffer written right after each send */
	vm_address_t b = 0;
	vm_allocate(mach_task_self(), &b, 512 * 1024, VM_FLAGS_ANYWHERE);
	for (uint32_t round = 0; round < ROUNDS; round++) {
		const vm_size_t size = (vm_size_t)(16 * 1024) << (round % 6);
		const uint32_t v = 0x30 + round % 150;
		memset((void *)b, (int)v, size);
		send(parent, M_OOL, v, round, MACH_PORT_NULL, 0, (void *)b, size);
		memset((void *)b, (int)(v + 1), size);
		if (receive(mine, &r) != KERN_SUCCESS) {
			_exit(5);
		}
	}
	send(parent, M_END, 0, 0, MACH_PORT_NULL, 0, NULL, 0);
	_exit(0);
}

static void
test_cross(void)
{
	mach_port_t port = MACH_PORT_NULL, kid = MACH_PORT_NULL;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (mach_ports_register(mach_task_self(), &port, 1) != KERN_SUCCESS) {
		say("SHMTEST2: cross: mach_ports_register FAIL");
		return;
	}
	const pid_t pid = fork();
	if (pid == 0) {
		child();
	}
	if (pid < 0) {
		say("SHMTEST2: cross: fork FAIL");
		return;
	}
	rmsg_t r;
	int bad = 0, badool = 0, rounds = 0, oolrounds = 0, entries = 0;
	vm_address_t map = 0;
	for (;;) {
		if (receive(port, &r) != KERN_SUCCESS) {
			say("SHMTEST2: cross: no message from the child FAIL");
			bad++;
			break;
		}
		const uint32_t kind = r.m.kind;
		if (kind == M_HELLO) {
			kid = r.m.p.name;
			continue;
		}
		if (kind == M_END) {
			break;
		}
		if (kind == M_ENTRY) {
			if (map) {
				vm_deallocate(mach_task_self(), map, BUFSIZE);
			}
			map = 0;
			if (vm_map(mach_task_self(), &map, BUFSIZE, 0, VM_FLAGS_ANYWHERE, r.m.p.name, 0, FALSE, VM_PROT_READ,
			    VM_PROT_READ, VM_INHERIT_NONE) != KERN_SUCCESS) {
				say("SHMTEST2: cross: vm_map of the child's entry FAIL");
				bad++;
				map = 0;
			} else {
				entries++;
				const long at = differs((const volatile unsigned char *)map, BUFSIZE, (unsigned char)r.m.value);
				if (at >= 0 && bad++ < 5) {
					say("SHMTEST2: cross: round %u, new entry, byte %ld is 0x%02x, wanted 0x%02x FAIL", r.m.round, at,
					    ((volatile unsigned char *)map)[at], r.m.value);
				}
			}
			mach_port_deallocate(mach_task_self(), r.m.p.name);
		} else if (kind == M_WROTE && map) {
			rounds++;
			const long at = differs((const volatile unsigned char *)map, BUFSIZE, (unsigned char)r.m.value);
			if (at >= 0 && bad++ < 5) {
				say("SHMTEST2: cross: round %u, byte %ld is 0x%02x, the child wrote 0x%02x FAIL", r.m.round, at,
				    ((volatile unsigned char *)map)[at], r.m.value);
			}
		} else if (kind == M_OOL) {
			oolrounds++;
			const long at = differs((const volatile unsigned char *)r.m.o.address, r.m.o.size, (unsigned char)r.m.value);
			if (at >= 0 && badool++ < 5) {
				say("SHMTEST2: oolx: round %u (%u KiB), byte %ld is 0x%02x, sent 0x%02x FAIL", r.m.round, r.m.o.size >> 10,
				    at, ((volatile unsigned char *)r.m.o.address)[at], r.m.value);
			}
			vm_deallocate(mach_task_self(), (vm_address_t)r.m.o.address, r.m.o.size);
		}
		if (kid != MACH_PORT_NULL) {
			send(kid, M_OK, 0, 0, MACH_PORT_NULL, 0, NULL, 0);
		}
	}
	int status = 0;
	waitpid(pid, &status, 0);
	say("SHMTEST2: cross: %d entries, %d rounds, %d not as the child wrote (child exit %d)", entries, rounds, bad,
	    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	say("SHMTEST2: oolx: %d messages, %d not as sent", oolrounds, badool);
}

/* ---- fork ----------------------------------------------------------------------------------------------------------- */
static void
test_fork(void)
{
	int bad = 0;
	for (int r = 0; r < 50; r++) {
		const vm_size_t size = 256 * 1024;
		vm_address_t a = 0;
		vm_allocate(mach_task_self(), &a, size, VM_FLAGS_ANYWHERE);
		memset((void *)a, 0x31, size);
		const pid_t pid = fork();
		if (pid == 0) {
			const long seen = differs((const volatile unsigned char *)a, size, 0x31);
			memset((void *)a, 0x32, size);
			usleep(2000);
			_exit(seen >= 0 ? 7 : 0);
		}
		memset((void *)a, 0x33, size / 2);   /* the parent writes too, while the child does */
		int status = 0;
		waitpid(pid, &status, 0);
		const long at1 = differs((const volatile unsigned char *)a, size / 2, 0x33);
		const long at2 = differs((const volatile unsigned char *)a + size / 2, size / 2, 0x31);
		if (at1 >= 0 || at2 >= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			if (bad < 5) {
				say("SHMTEST2: fork: round %d: parent %ld %ld, child exit %d FAIL", r, at1, at2,
				    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
			}
			bad++;
		}
		vm_deallocate(mach_task_self(), a, size);
	}
	say("SHMTEST2: fork: 50 rounds, %d with writes crossing", bad);
}

/* ---- order ---------------------------------------------------------------------------------------------------------- */
#define ORDER_N 5000000u
struct pair {
	_Alignas(128) atomic_uint seq;
	_Alignas(128) volatile unsigned data[16];
	unsigned bad, seen;
};

static void *
writer(void *arg)
{
	struct pair *p = arg;
	for (unsigned i = 1; i <= ORDER_N; i++) {
		for (int k = 0; k < 16; k++) {
			p->data[k] = i;
		}
		atomic_store_explicit(&p->seq, i, memory_order_release);
	}
	return NULL;
}

static void *
reader(void *arg)
{
	struct pair *p = arg;
	unsigned last = 0;
	while (last < ORDER_N) {
		const unsigned s = atomic_load_explicit(&p->seq, memory_order_acquire);
		if (s == last) {
			continue;
		}
		for (int k = 0; k < 16; k++) {
			if (p->data[k] < s) {
				p->bad++;
				break;
			}
		}
		p->seen++;
		last = s;
	}
	return NULL;
}

static void
test_order(void)
{
	struct pair *pairs = calloc(4, sizeof(struct pair));
	pthread_t t[8];
	for (int i = 0; i < 4; i++) {
		pthread_create(&t[2 * i], NULL, writer, &pairs[i]);
		pthread_create(&t[2 * i + 1], NULL, reader, &pairs[i]);
	}
	unsigned bad = 0, seen = 0;
	for (int i = 0; i < 8; i++) {
		pthread_join(t[i], NULL);
	}
	for (int i = 0; i < 4; i++) {
		bad += pairs[i].bad;
		seen += pairs[i].seen;
	}
	say("SHMTEST2: order: 4 pairs x %u writes, %u reads checked, %u saw older words than the number", ORDER_N, seen, bad);
	free(pairs);
}

/* ---- time ----------------------------------------------------------------------------------------------------------- */
static void
test_time(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	struct timespec m0, m1;
	clock_gettime(CLOCK_MONOTONIC, &m0);
	const uint64_t t0 = mach_absolute_time(), c0 = mach_continuous_time();
	sleep(2);
	const uint64_t t1 = mach_absolute_time(), c1 = mach_continuous_time();
	clock_gettime(CLOCK_MONOTONIC, &m1);
	const double mono = (double)(m1.tv_sec - m0.tv_sec) + (double)(m1.tv_nsec - m0.tv_nsec) / 1e9;
	const double abs_s = (double)(t1 - t0) * tb.numer / tb.denom / 1e9;
	say("SHMTEST2: time: timebase %u/%u, %llu ticks (%llu continuous) in %.4f s of CLOCK_MONOTONIC, %.0f ticks/s, "
	    "mach time %.4f s (ratio %.4f)", tb.numer, tb.denom, (unsigned long long)(t1 - t0),
	    (unsigned long long)(c1 - c0), mono, (double)(t1 - t0) / mono, abs_s, abs_s / mono);
}

int
main(void)
{
	con = open("/dev/console", O_WRONLY);
	if (con >= 0) {
		dup2(con, 1);
		dup2(con, 2);
	}
	sleep(20);
	say("SHMTEST2: start (pid %d)", getpid());
	test_time();
	test_zero();
	test_cross();
	test_fork();
	test_order();
	say("SHMTEST2: done");
	for (;;) {
		pause();
	}
}
