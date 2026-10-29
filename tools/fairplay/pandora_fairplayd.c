/*
 * pandora_fairplayd: in place of fairplayd.H2 (boot-arg sweetexecswap=fairplayd.H2:/usr/libexec/pandora_fairplayd),
 * the job's three services answered at once with "no such routine".
 *
 * Apple's fairplayd needs the FairPlayIOKit driver and the SoC's FairPlay keys. Without them it exits at start
 * (with the 0314 stand-in driver too: its calls 22, 23 and 21 are refused and it exits), launchd starts it again every
 * 10 s, and requests to its services wait in launchd's queue. locationd's FairPlay signer (CLMescalSigner
 * initInSilo:) sends MIG 1203 to com.apple.unfreed (host special port 17) with no timeout and waits for good, and
 * with it locationd's SPI service: the Astronomy poster's editor waits there for its location authorization
 * (post3, post4, post5c).
 *
 * This program checks in the services of the fairplayd.H2 job (launchd took it for that job) and answers every
 * message that carries a reply port with a MIG error reply (MIG_BAD_ID, id + 100), so the caller's MIG stub
 * returns an error instead of waiting. It stays running (the job is KeepAlive). The first 64 messages are logged
 * (os_log, and the console when it can be opened).
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* libSystem (libxpc) exports it; the iOS SDK has no servers/bootstrap.h. */
extern kern_return_t bootstrap_check_in(mach_port_t bp, const char *service_name, mach_port_t *sp);

static const char *const services[] = {
	"com.apple.unfreed",
	"com.apple.fairplayd",
	"com.apple.fairplayd.versioned",
};

static int console = -1;

static void note(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	os_log(OS_LOG_DEFAULT, "pandora_fairplayd: %{public}s", line);
	if (console >= 0) {
		dprintf(console, "PANDORA FAIRPLAYD: %s\n", line);
	}
}

int main(void)
{
	console = open("/dev/console", O_WRONLY | O_NOCTTY);
	mach_port_t set = MACH_PORT_NULL;
	kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &set);
	if (kr != KERN_SUCCESS) {
		note("no port set: %#x", kr);
		return 1;
	}
	unsigned held = 0;
	for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		mach_port_t port = MACH_PORT_NULL;
		kr = bootstrap_check_in(bootstrap_port, services[i], &port);
		if (kr == KERN_SUCCESS) {
			kr = mach_port_move_member(mach_task_self(), port, set);
			held += kr == KERN_SUCCESS;
		}
		note("%s: check-in %#x", services[i], kr);
	}
	if (held == 0) {
		return 1;
	}

	static union {
		mach_msg_header_t head;
		uint8_t bytes[64 * 1024];
	} in;
	unsigned seen = 0;
	for (;;) {
		kr = mach_msg(&in.head, MACH_RCV_MSG, 0, sizeof(in), set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS) {
			note("receive %#x", kr);   /* a message too large is destroyed by the kernel; its sender sees that */
			continue;
		}
		const mach_msg_bits_t reply_type = MACH_MSGH_BITS_REMOTE(in.head.msgh_bits);
		const mach_port_t reply = in.head.msgh_remote_port;
		if (seen < 64) {
			seen++;
			note("message %d size %u on %#x, reply port %#x", in.head.msgh_id, in.head.msgh_size,
			    in.head.msgh_local_port, reply);
		}
		if (reply != MACH_PORT_NULL && reply_type != 0) {
			mig_reply_error_t out;
			memset(&out, 0, sizeof(out));
			out.Head.msgh_bits = MACH_MSGH_BITS(reply_type, 0);
			out.Head.msgh_size = sizeof(out);
			out.Head.msgh_remote_port = reply;
			out.Head.msgh_local_port = MACH_PORT_NULL;
			out.Head.msgh_id = in.head.msgh_id + 100;
			out.NDR = NDR_record;
			out.RetCode = MIG_BAD_ID;
			kr = mach_msg(&out.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(out), 0, MACH_PORT_NULL, 100,
			    MACH_PORT_NULL);
			if (kr == MACH_MSG_SUCCESS) {
				in.head.msgh_remote_port = MACH_PORT_NULL;   /* the reply right went with the answer */
			}
		}
		mach_msg_destroy(&in.head);
	}
}
