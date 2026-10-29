/*
 * pandora_sink: runs in place of a launchd daemon the kernel no longer starts (crashed sweetcrashstop times this boot,
 * or listed in sweetnoexec; kernel 0330 execs /usr/libexec/pandora_sink instead, argv and environment unchanged). It
 * looks up its job in /System/Library/xpc/launchd.plist (by XPC_SERVICE_NAME, which launchd sets to the job's label,
 * else by the program path in argv[0]), checks in every Mach service of the job, and answers each request at once:
 * a MIG request carrying a reply port gets a MIG error reply (MIG_BAD_ID), anything else (XPC) is destroyed, which
 * gives the sender's reply port a send-once notification and its XPC call an interrupted connection. Before, a
 * daemon that was not started left its services registered with launchd and their requests queued for good: the
 * Settings Notifications page waited on CommCenter (post25: viewDidLoad in _CTServerConnectionCopyCellBroadcast-
 * AlertTypesForAllSIMSlots, xpc_connection_send_message_with_reply_sync). It stays running (jobs are often
 * KeepAlive). The first 64 requests are logged (os_log). tools/fairplay/pandora_fairplayd.c is the same for one job.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* libSystem (libxpc) exports it; the iOS SDK has no servers/bootstrap.h. */
extern kern_return_t bootstrap_check_in(mach_port_t bp, const char *service_name, mach_port_t *sp);

#define LAUNCHD_PLIST "/System/Library/xpc/launchd.plist"
#define MAX_SERVICES 64

static os_log_t sink_log;
static char job[128] = "?";

static CFDictionaryRef
load_plist(void)
{
	int fd = open(LAUNCHD_PLIST, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	UInt8 *bytes = malloc((size_t)st.st_size);
	ssize_t got = bytes ? read(fd, bytes, (size_t)st.st_size) : -1;
	close(fd);
	if (got != st.st_size) {
		free(bytes);
		return NULL;
	}
	CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, bytes, got, kCFAllocatorMalloc);
	CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist && CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		plist = NULL;
	}
	return plist;
}

static bool
string_is(CFTypeRef value, const char *text)
{
	char buf[1024];
	return text && value && CFGetTypeID(value) == CFStringGetTypeID() &&
	       CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8) && strcmp(buf, text) == 0;
}

/* The job whose Label is LABEL, else whose Program or first ProgramArguments entry is PATH. */
static CFDictionaryRef
find_job(CFDictionaryRef plist, const char *label, const char *path)
{
	CFDictionaryRef daemons = CFDictionaryGetValue(plist, CFSTR("LaunchDaemons"));
	if (!daemons || CFGetTypeID(daemons) != CFDictionaryGetTypeID()) {
		return NULL;
	}
	const CFIndex n = CFDictionaryGetCount(daemons);
	const void **jobs = calloc((size_t)n, sizeof(void *));
	CFDictionaryRef by_label = NULL, by_path = NULL;
	CFDictionaryGetKeysAndValues(daemons, NULL, jobs);
	for (CFIndex i = 0; i < n; i++) {
		CFDictionaryRef j = jobs[i];
		if (!j || CFGetTypeID(j) != CFDictionaryGetTypeID()) {
			continue;
		}
		if (!by_label && string_is(CFDictionaryGetValue(j, CFSTR("Label")), label)) {
			by_label = j;
		}
		CFTypeRef program = CFDictionaryGetValue(j, CFSTR("Program"));
		CFArrayRef args = CFDictionaryGetValue(j, CFSTR("ProgramArguments"));
		if (!program && args && CFGetTypeID(args) == CFArrayGetTypeID() && CFArrayGetCount(args) > 0) {
			program = CFArrayGetValueAtIndex(args, 0);
		}
		if (!by_path && string_is(program, path)) {
			by_path = j;
		}
	}
	free(jobs);
	return by_label ? by_label : by_path;
}

int
main(int argc, char **argv)
{
	sink_log = os_log_create("com.data-a-live.sink", "sink");
	const char *label = getenv("XPC_SERVICE_NAME");
	const char *path = argc > 0 ? argv[0] : NULL;
	mach_port_t set = MACH_PORT_NULL;
	char names[MAX_SERVICES][128];
	mach_port_t ports[MAX_SERVICES];
	unsigned count = 0;

	CFDictionaryRef plist = load_plist();
	CFDictionaryRef j = plist ? find_job(plist, label, path) : NULL;
	if (j) {
		CFTypeRef l = CFDictionaryGetValue(j, CFSTR("Label"));
		if (l && CFGetTypeID(l) == CFStringGetTypeID()) {
			CFStringGetCString(l, job, sizeof(job), kCFStringEncodingUTF8);
		}
	}
	CFDictionaryRef services = j ? CFDictionaryGetValue(j, CFSTR("MachServices")) : NULL;
	if (services && CFGetTypeID(services) == CFDictionaryGetTypeID() &&
	    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &set) == KERN_SUCCESS) {
		const CFIndex n = CFDictionaryGetCount(services);
		const void **keys = calloc((size_t)n, sizeof(void *));
		CFDictionaryGetKeysAndValues(services, keys, NULL);
		for (CFIndex i = 0; i < n && count < MAX_SERVICES; i++) {
			char name[128];
			mach_port_t port = MACH_PORT_NULL;
			if (!keys[i] || CFGetTypeID(keys[i]) != CFStringGetTypeID() ||
			    !CFStringGetCString(keys[i], name, sizeof(name), kCFStringEncodingUTF8)) {
				continue;
			}
			kern_return_t kr = bootstrap_check_in(bootstrap_port, name, &port);
			if (kr == KERN_SUCCESS && mach_port_insert_member(mach_task_self(), port, set) == KERN_SUCCESS) {
				strlcpy(names[count], name, sizeof(names[count]));
				ports[count++] = port;
			} else {
				os_log_error(sink_log, "pandora-sink %{public}s: check-in of %{public}s -> %#x", job, name, kr);
			}
		}
		free(keys);
	}
	os_log_error(sink_log, "pandora-sink: in place of %{public}s (%{public}s, label %{public}s): %u services checked in",
	    job, path ? path : "?", label ? label : "none", count);
	if (!count) {
		for (;;) {
			pause();
		}
	}

	unsigned logged = 0;
	for (;;) {
		union {
			mach_msg_header_t head;
			uint8_t bytes[16384 + MAX_TRAILER_SIZE];
		} in;
		kern_return_t kr = mach_msg(&in.head, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof(in), set,
		    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr == MACH_RCV_TOO_LARGE) {
			/* Too big for the buffer: take it again into a big enough one and drop it. */
			const mach_msg_size_t size = in.head.msgh_size + MAX_TRAILER_SIZE;
			mach_msg_header_t *big = malloc(size);
			if (big && mach_msg(big, MACH_RCV_MSG, 0, size, set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) ==
			    KERN_SUCCESS) {
				mach_msg_destroy(big);
			}
			free(big);
			continue;
		}
		if (kr != KERN_SUCCESS) {
			continue;
		}
		const char *service = "?";
		for (unsigned i = 0; i < count; i++) {
			if (ports[i] == in.head.msgh_local_port) {
				service = names[i];
			}
		}
		const mach_msg_bits_t reply_type = MACH_MSGH_BITS_REMOTE(in.head.msgh_bits);
		const mach_port_t reply = in.head.msgh_remote_port;
		const bool mig = in.head.msgh_id > 0 && in.head.msgh_id < 0x10000000;
		if (logged < 64) {
			logged++;
			os_log_error(sink_log, "pandora-sink %{public}s: message %d on %{public}s, %{public}s", job,
			    in.head.msgh_id, service, mig && reply ? "MIG error reply" : "dropped");
		}
		if (mig && reply != MACH_PORT_NULL && reply_type != 0) {
			mig_reply_error_t out;
			memset(&out, 0, sizeof(out));
			out.Head.msgh_bits = MACH_MSGH_BITS(reply_type, 0);
			out.Head.msgh_size = sizeof(out);
			out.Head.msgh_remote_port = reply;
			out.Head.msgh_id = in.head.msgh_id + 100;
			out.NDR = NDR_record;
			out.RetCode = MIG_BAD_ID;
			kr = mach_msg(&out.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(out), 0, MACH_PORT_NULL, 100,
			    MACH_PORT_NULL);
			if (kr == KERN_SUCCESS) {
				in.head.msgh_remote_port = MACH_PORT_NULL;   /* the reply right went with the answer */
				in.head.msgh_bits &= ~MACH_MSGH_BITS_REMOTE_MASK;
			}
		}
		mach_msg_destroy(&in.head);
	}
}
