/*
 * shmtest: does memory shared the way QuartzCore shares a backing store stay shared?
 *
 * Settings' rows show for a moment the words of another row (the one tapped before), with the render server on the
 * GPU or on the CPU and with one CPU core (PLAN.md, shot1-shot11). An app's layer contents are CA::Render::Shmem
 * bitmaps: QuartzCore allocates them with vm_allocate (VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE, tag 0x33,
 * CA::Render::Shmem::new_shmem), hands them over with mach_make_memory_entry_64 (MAP_MEM_VM_SHARE |
 * MAP_MEM_USE_DATA_ADDR, read or read-write: CA::Render::Shmem::port), keeps unused ones volatile and makes them
 * nonvolatile again for reuse (Shmem::set_volatile), and asks whether the render server still uses one through the
 * word at its start (buffer_is_unused). Here one task does each step and checks whether a second mapping of the
 * entry (as the render server maps it) sees what the first mapping wrote:
 *   1. read-only entry, mapped shared: a write through the first mapping, seen through the second?
 *   2. the same after volatile -> nonvolatile (the purge state on the way back is printed)
 *   3. the same with a read-write entry, and a write through the second mapping seen by the first (the in-use word)
 *   4. control: the entry mapped with copy = TRUE, which by design keeps what it saw (expected NOT to follow)
 *   5. 200 rounds of 1-3 with buffers freed and allocated again in between (addresses reused)
 *   6-7. copy-on-write: vm_read copies and out-of-line message bytes (below), 400 rounds of 16 KiB to 512 KiB
 * (iOS has no mach_vm_* calls in its SDK; the vm_* ones are the same 64-bit calls here.)
 * Run in place of bluetoothd (sweetexecswap); "SHMTEST:" lines on /dev/console (the kernel log).
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef MAP_MEM_VM_SHARE
#define MAP_MEM_VM_SHARE        0x400000
#endif
#ifndef MAP_MEM_USE_DATA_ADDR
#define MAP_MEM_USE_DATA_ADDR   0x100000
#endif

static int con = -1, failures;

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

/* First byte of SIZE bytes at P that is not V, or -1. */
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

static void
expect(const char *what, const volatile unsigned char *p, size_t size, unsigned char v, int want_same, int quiet)
{
	const long at = differs(p, size, v);
	const int same = at < 0;
	if (same != want_same) {
		failures++;
	}
	if (!quiet || same != want_same) {
		say("SHMTEST: %s: %s%s (byte %ld is 0x%02x)", what, same ? "follows" : "does NOT follow",
		    same == want_same ? "" : " FAIL", at, at < 0 ? v : p[at]);
	}
}

/* One buffer as QuartzCore makes it; the second mapping as the render server maps it. */
static int
round_trip(int round, vm_prot_t prot, int quiet)
{
	const vm_size_t size = 64 * 1024;
	vm_address_t a = 0, b = 0;
	kern_return_t kr = vm_allocate(mach_task_self(), &a, size,
	    VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE | VM_MAKE_TAG(0x33));
	if (kr != KERN_SUCCESS) {
		say("SHMTEST: round %d: vm_allocate -> %d FAIL", round, kr);
		failures++;
		return -1;
	}
	memset((void *)a, 0xA1, size);
	memory_object_size_t esize = size;
	mach_port_t port = MACH_PORT_NULL;
	kr = mach_make_memory_entry_64(mach_task_self(), &esize, a, prot | MAP_MEM_VM_SHARE | MAP_MEM_USE_DATA_ADDR,
	    &port, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		say("SHMTEST: round %d: make_memory_entry (prot %d) -> %d FAIL", round, prot, kr);
		failures++;
		vm_deallocate(mach_task_self(), a, size);
		return -1;
	}
	kr = vm_map(mach_task_self(), &b, esize, 0, VM_FLAGS_ANYWHERE, port, 0, FALSE, prot, prot, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		say("SHMTEST: round %d: vm_map (prot %d) -> %d FAIL", round, prot, kr);
		failures++;
		mach_port_deallocate(mach_task_self(), port);
		vm_deallocate(mach_task_self(), a, size);
		return -1;
	}
	char what[96];
	const volatile unsigned char *pb = (const volatile unsigned char *)b;
	snprintf(what, sizeof what, "round %d prot %d: first contents in the second mapping", round, prot);
	expect(what, pb, size, 0xA1, 1, quiet);
	memset((void *)a, 0xB2, size);
	snprintf(what, sizeof what, "round %d prot %d: a later write", round, prot);
	expect(what, pb, size, 0xB2, 1, quiet);
	int state = VM_PURGABLE_VOLATILE;
	kr = vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &state);
	state = VM_PURGABLE_NONVOLATILE;
	kern_return_t kr2 = vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &state);
	if (!quiet || kr || kr2) {
		say("SHMTEST: round %d prot %d: volatile -> %d, nonvolatile -> %d, was %s", round, prot, kr, kr2,
		    state == VM_PURGABLE_EMPTY ? "purged" : "kept");
	}
	memset((void *)a, 0xC3, size);
	snprintf(what, sizeof what, "round %d prot %d: a write after volatile and back", round, prot);
	expect(what, pb, size, 0xC3, 1, quiet);
	if (prot & VM_PROT_WRITE) {
		memset((void *)b, 0xD4, 4);         /* the render server's in-use word */
		snprintf(what, sizeof what, "round %d: a write through the second mapping, seen by the first", round);
		expect(what, (const volatile unsigned char *)a, 4, 0xD4, 1, quiet);
	}
	vm_deallocate(mach_task_self(), b, esize);
	mach_port_deallocate(mach_task_self(), port);
	vm_deallocate(mach_task_self(), a, size);
	return 0;
}

/* Control: copy = TRUE takes what the memory holds then and keeps it. */
static void
copy_control(void)
{
	const vm_size_t size = 16 * 1024;
	vm_address_t a = 0, b = 0;
	if (vm_allocate(mach_task_self(), &a, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE) != KERN_SUCCESS) {
		return;
	}
	memset((void *)a, 0x11, size);
	memory_object_size_t esize = size;
	mach_port_t port = MACH_PORT_NULL;
	if (mach_make_memory_entry_64(mach_task_self(), &esize, a, VM_PROT_READ | MAP_MEM_VM_SHARE | MAP_MEM_USE_DATA_ADDR,
	    &port, MACH_PORT_NULL) == KERN_SUCCESS &&
	    vm_map(mach_task_self(), &b, esize, 0, VM_FLAGS_ANYWHERE, port, 0, TRUE, VM_PROT_READ, VM_PROT_READ,
	    VM_INHERIT_NONE) == KERN_SUCCESS) {
		memset((void *)a, 0x22, size);
		expect("control, copy = TRUE: a later write", (const volatile unsigned char *)b, size, 0x22, 0, 0);
		vm_deallocate(mach_task_self(), b, esize);
	}
	if (port != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), port);
	}
	vm_deallocate(mach_task_self(), a, size);
}

/*
 * Copy-on-write. QuartzCore sends each commit's encoded layer changes as out-of-line memory (a virtual copy), then
 * goes on writing its encoder buffers; the receiver must see the bytes as they were when the message was sent.
 * 6. vm_read gives a copy of SIZE bytes; then the source is written: the copy must keep the old bytes, and a write to
 *    the copy must not reach the source.
 * 7. a message to ourselves with SIZE bytes out of line (MACH_MSG_VIRTUAL_COPY, not deallocated), the source written
 *    right after the send and before the receive: the received bytes must be the sent ones.
 */
static void
cow_round(int round, vm_size_t size, int quiet)
{
	vm_address_t src = 0;
	if (vm_allocate(mach_task_self(), &src, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
		return;
	}
	memset((void *)src, 0x55, size);
	vm_offset_t copy = 0;
	mach_msg_type_number_t count = 0;
	char what[96];
	if (vm_read(mach_task_self(), src, size, &copy, &count) == KERN_SUCCESS) {
		memset((void *)src, 0x66, size);
		snprintf(what, sizeof what, "cow %d (%lu KiB): vm_read copy after the source is written", round,
		    (unsigned long)(size >> 10));
		expect(what, (const volatile unsigned char *)copy, size, 0x55, 1, quiet);
		memset((void *)copy, 0x77, size);
		snprintf(what, sizeof what, "cow %d: the source after the copy is written", round);
		expect(what, (const volatile unsigned char *)src, size, 0x66, 1, quiet);
		vm_deallocate(mach_task_self(), copy, count);
	} else {
		say("SHMTEST: cow %d: vm_read FAIL", round);
		failures++;
	}
	mach_port_t port = MACH_PORT_NULL;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS) {
		struct {
			mach_msg_header_t h;
			mach_msg_body_t b;
			mach_msg_ool_descriptor_t d;
		} msg;
		memset(&msg, 0, sizeof msg);
		memset((void *)src, 0x88, size);
		msg.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
		msg.h.msgh_size = sizeof msg;
		msg.h.msgh_remote_port = port;
		msg.h.msgh_id = 0x5348;
		msg.b.msgh_descriptor_count = 1;
		msg.d.address = (void *)src;
		msg.d.size = (mach_msg_size_t)size;
		msg.d.deallocate = FALSE;
		msg.d.copy = MACH_MSG_VIRTUAL_COPY;
		msg.d.type = MACH_MSG_OOL_DESCRIPTOR;
		kern_return_t kr = mach_msg(&msg.h, MACH_SEND_MSG, sizeof msg, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
		memset((void *)src, 0x99, size);    /* the sender goes on with its buffer */
		struct {
			mach_msg_header_t h;
			mach_msg_body_t b;
			mach_msg_ool_descriptor_t d;
			mach_msg_trailer_t t;
		} in;
		memset(&in, 0, sizeof in);
		if (kr == KERN_SUCCESS) {
			kr = mach_msg(&in.h, MACH_RCV_MSG, 0, sizeof in, port, 0, MACH_PORT_NULL);
		}
		if (kr != KERN_SUCCESS) {
			say("SHMTEST: cow %d: message -> %d FAIL", round, kr);
			failures++;
		} else {
			snprintf(what, sizeof what, "cow %d: out-of-line bytes received after the sender wrote its buffer", round);
			expect(what, (const volatile unsigned char *)in.d.address, size, 0x88, 1, quiet);
			vm_deallocate(mach_task_self(), (vm_address_t)in.d.address, in.d.size);
		}
		mach_port_destroy(mach_task_self(), port);
	}
	vm_deallocate(mach_task_self(), src, size);
}

int
main(void)
{
	con = open("/dev/console", O_WRONLY);
	if (con >= 0) {
		dup2(con, 1);
		dup2(con, 2);
	}
	sleep(20);                  /* after the boot's busiest part */
	say("SHMTEST: start (pid %d)", getpid());
	round_trip(0, VM_PROT_READ, 0);
	round_trip(0, VM_PROT_READ | VM_PROT_WRITE, 0);
	copy_control();
	for (int r = 1; r <= 200; r++) {
		round_trip(r, (r & 1) ? VM_PROT_READ : VM_PROT_READ | VM_PROT_WRITE, 1);
	}
	cow_round(0, 256 * 1024, 0);
	for (int r = 1; r <= 400; r++) {
		cow_round(r, (vm_size_t)(16 * 1024) << (r % 6), 1);    /* 16 KiB to 512 KiB */
	}
	say("SHMTEST: done, %d failures", failures);
	for (;;) {
		pause();                /* launchd would start it again at once */
	}
}
