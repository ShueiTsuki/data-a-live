/*
 * The speaker as iOS's built-in audio device, step 1 (0281): our own IOAudio2Device and IOAudio2DeviceUserClient.
 * Apple's IOAudio2Family is not prelinked here (tools/mkprelink.py keeps AMFI and its libraries only), so the class
 * names are free; CoreAudio's HALS_IOA2Manager looks devices up with IOServiceMatching("IOAudio2Device").
 *
 * The contract, from the iPhone X kernelcache (20H392) and its CoreAudio (PLAN 2026-10-28, iosaudiocheck2-4):
 *  - the description is in the device's registry properties, lower-case keys with spaces, set by Apple's hardware
 *    subclasses (AppleARMIISAudio, AppleEmbeddedAudio) with IOAudio2Family's helpers: numbers are 32 bits, sample
 *    rates 64 bits in 32.32 fixed point (HALS_IOA2UCDevice::GetNominalSampleRate); "transport type" 'bltn';
 *    "output streams" an array of stream dictionaries ("stream ID", "starting channel", "terminal type",
 *    "current format", "available formats"); CoreAudio needs "device name" and "device UID";
 *  - user client: 13 methods (old-style table, every one a device virtual), one trap (DoIO, a no-op for built-in
 *    devices), clientMemoryForType type >> 28: 0 = the engine status page (u64 sample time, host time, seed, read
 *    twice by HALS_IOA2Engine2::GetZeroTimeStamp), 1 = the IO buffer of stream (type & 0x0fffffff);
 *  - order: open, map status and IO buffers, method 0 (engine start) and 10 (StartIOWithFlags), then the IO thread
 *    reads zero time stamps; 1 and 11 stop.
 * Step 1 plays nothing: every call is logged, IO runs on a clock of our own (a zero time stamp each time the ring
 * wraps) and the ring's peak is logged each second, to see that mediaserverd finds the device, opens it, maps the
 * buffers and writes into them. Boot-arg pandora_audio=1.
 */
#include <pexpert/arm64/board_config.h>
#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>

#define PA_4CC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PA_RATE           48000u
#define PA_CHANNELS       2u
#define PA_BYTES_FRAME    (PA_CHANNELS * 4u)                 /* 32-bit signed integer, interleaved */
#define PA_RING_FRAMES    16384u
#define PA_RING_BYTES     (PA_RING_FRAMES * PA_BYTES_FRAME)
#define PA_STREAM_ID      1u
#define PA_STATUS_BYTES   4096u                              /* CoreAudio wants at least 40 */
#define PA_OUT_LATENCY    480u
#define PA_OUT_SAFETY     960u

struct PaEngineStatus {                                      /* CoreAudio's IOAudio2EngineStatus {QQQQQ} */
	volatile uint64_t sample_time;
	volatile uint64_t host_time;
	volatile uint64_t seed;
	uint64_t unused[2];
};

static void
pa_num(OSDictionary *d, const char *key, uint64_t value, unsigned bits)
{
	OSNumber *n = OSNumber::withNumber(value, bits);
	if (n != nullptr) {
		d->setObject(key, n);
		n->release();
	}
}

/* A stream format as IOAudio2Family writes one: "current format" with "sample rate", an entry of "available
 * formats" with "min sample rate" / "max sample rate". */
static OSDictionary *
pa_format(bool range)
{
	OSDictionary *f = OSDictionary::withCapacity(10);
	if (f == nullptr) {
		return nullptr;
	}
	if (range) {
		pa_num(f, "min sample rate", (uint64_t)PA_RATE << 32, 64);
		pa_num(f, "max sample rate", (uint64_t)PA_RATE << 32, 64);
	} else {
		pa_num(f, "sample rate", (uint64_t)PA_RATE << 32, 64);
	}
	pa_num(f, "format ID", PA_4CC('l', 'p', 'c', 'm'), 32);
	pa_num(f, "format flags", 0x4u | 0x8u, 32);          /* kAudioFormatFlagIsSignedInteger | IsPacked */
	pa_num(f, "bytes per packet", PA_BYTES_FRAME, 32);
	pa_num(f, "frames per packet", 1, 32);
	pa_num(f, "bytes per frame", PA_BYTES_FRAME, 32);
	pa_num(f, "channels per frame", PA_CHANNELS, 32);
	pa_num(f, "bits per channel", 32, 32);
	return f;
}

static OSDictionary *
pa_stream(bool input)
{
	OSDictionary *s = OSDictionary::withCapacity(6);
	OSDictionary *cur = pa_format(false), *avail = pa_format(true);
	OSArray *list = avail == nullptr ? nullptr : OSArray::withObjects((const OSObject **)&avail, 1);
	if (s == nullptr || cur == nullptr || list == nullptr) {
		OSSafeReleaseNULL(s);
	} else {
		pa_num(s, "stream ID", PA_STREAM_ID, 32);
		pa_num(s, "starting channel", 1, 32);
		/* kAudioStreamTerminalTypeMicrophone / kAudioStreamTerminalTypeSpeaker */
		pa_num(s, "terminal type", input ? PA_4CC('m', 'i', 'c', 'r') : PA_4CC('s', 'p', 'k', 'r'), 32);
		s->setObject("current format", cur);
		s->setObject("available formats", list);
	}
	OSSafeReleaseNULL(cur);
	OSSafeReleaseNULL(avail);
	OSSafeReleaseNULL(list);
	return s;
}

class IOAudio2Device : public IOService {
	OSDeclareDefaultStructors(IOAudio2Device);

public:
	IOBufferMemoryDescriptor *status_ = nullptr;
	IOBufferMemoryDescriptor *ring_ = nullptr;
	IOLock *lock_ = nullptr;
	volatile bool running_ = false;
	volatile bool thread_on_ = false;
	uint64_t seed_ = 0;
	uint64_t t0_ = 0;
	volatile uint32_t traps_ = 0;
	bool hawking_ = false;                                   /* 0292: the "Hawking" input device, else "Speaker" */
	volatile bool routed_ = false;                           /* 0293: a client set a stream active */

	const char *tag(void) const { return hawking_ ? "Hawking" : "Speaker"; }
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	IOReturn startIO(void);
	void stopIO(void);
	void clockThread(void);
};

OSDefineMetaClassAndStructors(IOAudio2Device, IOService);

class IOAudio2DeviceUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOAudio2DeviceUserClient);

public:
	IOAudio2Device *device_ = nullptr;
	char name_[32] = {};
	bool started_ = false;

	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	IOReturn doIO(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
};

OSDefineMetaClassAndStructors(IOAudio2DeviceUserClient, IOUserClient);

/*
 * 0290/0293: when the speaker is published. VirtualAudio picks a route in full only when ports come or go, and the
 * speaker port is dropped from the default device's route while the ringer is off ("Culling route connection because
 * ringer switch is off"); a later ringer change does not bring it back (the default category's routing database entry
 * is not ringer dependent: "Ringer switch update." -> "Route did not change"). The ringer is off until SpringBoard
 * posts its state, about 11 s after backboardd reads the switch (SweetHIDButtons::copyEvent; aud13, unified log times
 * x52). So the speaker is published 3 s after that read and then watched: when VirtualAudio routes the default device
 * to it, it sets our stream active (aud19: "set stream active" 1 1 at "Route change succeeded"). Without that within
 * 5 s the speaker is plugged in again (a new instance, the old one terminated), up to 12 times. Fallbacks: first
 * publication at 120 s uptime if nobody reads the switch; boot-arg pandora_audio_delay=SECONDS a fixed time instead,
 * pandora_audio_now=1 at once.
 */
static volatile bool pa_ringer_known;
static IOAudio2Device *pa_speaker;                           /* the instance the supervisor publishes (retained) */

extern "C" void pandora_audio_ringer_known(void);
void
pandora_audio_ringer_known(void)
{
	pa_ringer_known = true;
}

static uint64_t
pa_uptime_ms(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* A new speaker instance, started (described) but not published. */
static IOAudio2Device *
pa_new_speaker(IOService *provider)
{
	IOAudio2Device *d = OSTypeAlloc(IOAudio2Device);
	if (d == nullptr || !d->init()) {
		OSSafeReleaseNULL(d);
		return nullptr;
	}
	if (!d->attach(provider)) {
		d->release();
		return nullptr;
	}
	if (!d->start(provider)) {
		d->detach(provider);
		d->release();
		return nullptr;
	}
	return d;
}

static void
pa_supervise(void *arg, wait_result_t)
{
	IOService *provider = static_cast<IOService *>(arg);
	int delay = 0, now = 0;
	(void)PE_parse_boot_argn("pandora_audio_delay", &delay, sizeof(delay));
	(void)PE_parse_boot_argn("pandora_audio_now", &now, sizeof(now));
	const char *why;
	if (now) {
		why = "at once";
	} else if (delay > 0) {
		while (pa_uptime_ms() < (uint64_t)delay * 1000) {
			IOSleep(200);
		}
		why = "fixed delay";
	} else {
		while (!pa_ringer_known && pa_uptime_ms() < 120000) {
			IOSleep(100);
		}
		if (pa_ringer_known) {
			IOSleep(3000);
			why = "3 s after the ring/silent switch was read";
		} else {
			why = "nobody read the ring/silent switch by 120 s";
		}
	}
	for (unsigned attempt = 1;; attempt++) {
		IOAudio2Device *d = pa_speaker;
		d->registerService();
		IOLog("PANDORA AUDIO Speaker: registered at %llu ms (%s, attempt %u)\n", pa_uptime_ms(), why, attempt);
		for (unsigned t = 0; t < 50 && !d->routed_; t++) {
			IOSleep(100);
		}
		if (d->routed_) {
			IOLog("PANDORA AUDIO Speaker: routed at %llu ms (attempt %u)\n", pa_uptime_ms(), attempt);
			break;
		}
		if (attempt == 12) {
			IOLog("PANDORA AUDIO Speaker: not routed after %u attempts, left as it is\n", attempt);
			break;
		}
		IOAudio2Device *n = pa_new_speaker(provider);
		if (n == nullptr) {
			IOLog("PANDORA AUDIO Speaker: no memory to plug it in again\n");
			break;
		}
		IOLog("PANDORA AUDIO Speaker: not routed within 5 s, plugging it in again\n");
		pa_speaker = n;
		d->terminate();
		d->release();
		why = "plugged in again";
	}
	provider->release();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

bool
IOAudio2Device::start(IOService *provider)
{
	int on = 0;
	(void)PE_parse_boot_argn("pandora_audio", &on, sizeof(on));
	if (!on || !IOService::start(provider)) {
		return false;
	}
	lock_ = IOLockAlloc();
	status_ = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, PA_STATUS_BYTES,
	    page_size);
	ring_ = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, PA_RING_BYTES,
	    page_size);
	OSDictionary *stream = pa_stream(false);
	OSArray *outs = stream == nullptr ? nullptr : OSArray::withObjects((const OSObject **)&stream, 1);
	OSArray *ins = OSArray::withCapacity(1), *controls = OSArray::withCapacity(1);
	if (lock_ == nullptr || status_ == nullptr || ring_ == nullptr || outs == nullptr || ins == nullptr ||
	    controls == nullptr) {
		IOLog("PANDORA AUDIO: no memory for the device\n");
		OSSafeReleaseNULL(stream);
		OSSafeReleaseNULL(outs);
		OSSafeReleaseNULL(ins);
		OSSafeReleaseNULL(controls);
		return false;
	}
	bzero(status_->getBytesNoCopy(), PA_STATUS_BYTES);
	bzero(ring_->getBytesNoCopy(), PA_RING_BYTES);
	/*
	 * 0292: a second instance (personality PandoraAudioRole = Hawking) is the iPhone's "Hawking" device, one input
	 * stream. aud18 (VirtualAudio at trace level, speaker published after the ringer was on): the route to the speaker
	 * is built ("Successfully Built concrete route { pap -> pspk }"), then dropped: the default device needs 8
	 * alternate devices, Hawking (vhaw, category chaw) among them "Mandatory", and its route [ phki / pap ] has no
	 * connected 'phki' port ("Recursion failed after skipping optional alternate VAD"), so the null device stays.
	 * VirtualAudio's Device_Hawking_Aspen makes 'phki' ("Hawking Input") and an input volume control from the device
	 * with UID "Hawking" (iPhone X: AppleSecondaryAudio "Hawking", /arm-io/mca4/mca4b/audio-codec-hawking). It is not
	 * tied to the ringer and is published at once. Its input stays silent.
	 */
	OSString *role = OSDynamicCast(OSString, getProperty("PandoraAudioRole"));
	hawking_ = role != nullptr && role->isEqualTo("Hawking");
	if (hawking_) {
		OSArray *t = outs;
		outs = ins;
		ins = t;
		OSSafeReleaseNULL(stream);
		stream = pa_stream(true);
		if (stream == nullptr) {
			IOLog("PANDORA AUDIO Hawking: no memory for the input stream\n");
			OSSafeReleaseNULL(outs);
			OSSafeReleaseNULL(ins);
			OSSafeReleaseNULL(controls);
			return false;
		}
		ins->replaceObject(0, stream);
		setProperty("device UID", "Hawking");
		setProperty("device name", "Hawking");
		setProperty("device manufacturer", "Pandora");
		setProperty("transport type", PA_4CC('b', 'l', 't', 'n'), 32);
		setProperty("sample rate", (unsigned long long)PA_RATE << 32, 64);
		setProperty("io buffer frame size", PA_RING_FRAMES, 32);
		setProperty("output latency", 0ull, 32);
		setProperty("output safety offset", 0ull, 32);
		setProperty("input latency", PA_OUT_LATENCY, 32);
		setProperty("input safety offset", PA_OUT_SAFETY, 32);
		setProperty("clock domain", PA_4CC('p', 'a', 'n', 'd'), 32);
		setProperty("output streams", outs);
		setProperty("input streams", ins);
		setProperty("controls", controls);
		OSSafeReleaseNULL(stream);
		OSSafeReleaseNULL(outs);
		OSSafeReleaseNULL(ins);
		OSSafeReleaseNULL(controls);
		registerService();
		IOLog("PANDORA AUDIO Hawking: registered: 1 input stream, %u Hz, %u channels, s32, ring %u frames\n", PA_RATE,
		    PA_CHANNELS, PA_RING_FRAMES);
		return true;
	}
	/* 0282: the UID VirtualAudio looks the speaker up by (aud1: HAL activated "PandoraBuiltInSpeaker" as device 33,
	 * but VirtualAudio built no route to it). Its table of physical devices takes the iPhone's UIDs: "Codec"
	 * (CS42L75: receiver, headphones, mics), "Speaker" (CS35L26), "Voice", "Reference", "Baseband Voice". */
	/* 0283: boot-arg pandora_audio_tt=1 makes it a Thunderbolt device, as sheng's plug-in on iPadOS 17
	 * (VirtualAudio there routes a Thunderbolt device as a generic driver device; built-in devices it takes only
	 * from the product's transducer database). aud2/aud3 (built-in, UID "Speaker"): still no route.
	 * 0291: aud12-aud14 (built-in "Speaker", VirtualAudio loaded): no category ever gets a route to the speaker port,
	 * whatever the ringer; the iPhone X routing database wants more than this one device. The Thunderbolt device
	 * takes its own UID so that VirtualAudio does not build the built-in speaker (Device_Speaker_Aspen) from it. */
	int tt = 0;
	(void)PE_parse_boot_argn("pandora_audio_tt", &tt, sizeof(tt));
	setProperty("device UID", tt == 1 ? "PandoraSpeaker" : "Speaker");
	setProperty("device name", "Speaker");
	setProperty("device manufacturer", "Pandora");
	setProperty("transport type", tt == 1 ? PA_4CC('t', 'h', 'u', 'n') : PA_4CC('b', 'l', 't', 'n'), 32);
	setProperty("sample rate", (unsigned long long)PA_RATE << 32, 64);
	setProperty("io buffer frame size", PA_RING_FRAMES, 32);
	setProperty("output latency", PA_OUT_LATENCY, 32);
	setProperty("output safety offset", PA_OUT_SAFETY, 32);
	setProperty("input latency", 0ull, 32);
	setProperty("input safety offset", 0ull, 32);
	setProperty("clock domain", PA_4CC('p', 'a', 'n', 'd'), 32);
	setProperty("output streams", outs);
	setProperty("input streams", ins);
	setProperty("controls", controls);
	OSSafeReleaseNULL(stream);
	OSSafeReleaseNULL(outs);
	OSSafeReleaseNULL(ins);
	OSSafeReleaseNULL(controls);
	/* 0293: published and watched by pa_supervise; an instance made to plug the speaker in again is published there. */
	if (pa_speaker == nullptr) {
		retain();
		pa_speaker = this;
		provider->retain();
		thread_t thread;
		if (kernel_thread_start(pa_supervise, provider, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			provider->release();
			registerService();
		}
	}
	IOLog("PANDORA AUDIO Speaker: made: 1 output stream, %u Hz, %u channels, s32, ring %u frames\n", PA_RATE,
	    PA_CHANNELS, PA_RING_FRAMES);
	return true;
}

void
IOAudio2Device::free(void)
{
	OSSafeReleaseNULL(status_);
	OSSafeReleaseNULL(ring_);
	if (lock_ != nullptr) {
		IOLockFree(lock_);
		lock_ = nullptr;
	}
	IOService::free();
}

/*
 * 0287 (aud7: VirtualAudio loaded, but "HALB_UCObject::SetProperty: got an error from the IORegistry, Error:
 * 0xE00002C7", "Unable to take hog mode.", "Could not add PhysicalDevice ID: 33"): clients set registry properties
 * on the device, first "exclusive access owner" (hog mode; HALS_IOA2Device::_ReadHogModeOwnerStore reads it back).
 * Every key is kept as given and logged.
 */
IOReturn
IOAudio2Device::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator *it = d == nullptr ? nullptr : OSCollectionIterator::withCollection(d);
	if (it == nullptr) {
		IOLog("PANDORA AUDIO: setProperties without a dictionary\n");
		return kIOReturnBadArgument;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	while (const OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) {
		OSObject *v = d->getObject(k);
		OSNumber *n = OSDynamicCast(OSNumber, v);
		OSString *s = OSDynamicCast(OSString, v);
		IOLog("PANDORA AUDIO %s: %s sets \"%s\" = %s%llu%s\n", tag(), name, k->getCStringNoCopy(),
		    n != nullptr ? "" : s != nullptr ? s->getCStringNoCopy() : v->getMetaClass()->getClassName(),
		    n != nullptr ? n->unsigned64BitValue() : 0ull, n != nullptr ? "" : " (not a number)");
		setProperty(k, v);
	}
	it->release();
	return kIOReturnSuccess;
}

IOReturn
IOAudio2Device::newUserClient(task_t owningTask, void *securityID, UInt32 type, __unused OSDictionary *properties,
    IOUserClient **handler)
{
	IOAudio2DeviceUserClient *uc = OSTypeAlloc(IOAudio2DeviceUserClient);
	if (uc == nullptr || !uc->initWithTask(owningTask, securityID, type)) {
		OSSafeReleaseNULL(uc);
		return kIOReturnNoMemory;
	}
	retain();                                                /* 0293: a speaker plugged in again is terminated */
	uc->device_ = this;
	proc_selfname(uc->name_, sizeof(uc->name_));
	if (!uc->attach(this) || !uc->start(this)) {
		uc->detach(this);
		uc->release();
		return kIOReturnError;
	}
	IOLog("PANDORA AUDIO %s: opened by %s (type %u)\n", tag(), uc->name_, (unsigned)type);
	*handler = uc;
	return kIOReturnSuccess;
}

/* Sample time of ring wrap N: N * ring; its host time: t0 + N * ring / rate seconds (exact, no drift). */
static uint64_t
pa_wrap_host(uint64_t t0, uint64_t n)
{
	uint64_t abs = 0;
	nanoseconds_to_absolutetime(n * PA_RING_FRAMES * 62500ull / 3ull, &abs);   /* 1e9 / 48000 = 62500 / 3 */
	return t0 + abs;
}

static void
pa_publish(IOAudio2Device *d, uint64_t n)
{
	PaEngineStatus *st = (PaEngineStatus *)d->status_->getBytesNoCopy();
	st->sample_time = n * PA_RING_FRAMES;
	st->host_time = pa_wrap_host(d->t0_, n);
	st->seed = d->seed_;
	__asm__ volatile ("dmb ish" ::: "memory");
}

void
IOAudio2Device::clockThread(void)
{
	uint64_t n = 0, last_log = mach_absolute_time(), one_s = 0;
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &one_s);
	while (running_) {
		IOSleep(5);
		const uint64_t now = mach_absolute_time();
		while (running_ && pa_wrap_host(t0_, n + 1) <= now) {
			pa_publish(this, ++n);
		}
		if (now - last_log >= one_s) {
			last_log = now;
			const int32_t *s = (const int32_t *)ring_->getBytesNoCopy();
			uint32_t peak = 0, nonzero = 0;
			for (uint32_t i = 0; i < PA_RING_FRAMES * PA_CHANNELS; i++) {
				const uint32_t a = s[i] < 0 ? (uint32_t)-(int64_t)s[i] : (uint32_t)s[i];
				peak = a > peak ? a : peak;
				nonzero += s[i] != 0;
			}
			IOLog("PANDORA AUDIO %s: running: wrap %llu, ring peak %#x, %u of %u samples nonzero, traps %u\n", tag(), n,
			    peak, nonzero, PA_RING_FRAMES * PA_CHANNELS, traps_);
		}
	}
	thread_on_ = false;
}

static void
pa_clock_thread(void *arg, wait_result_t)
{
	static_cast<IOAudio2Device *>(arg)->clockThread();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

IOReturn
IOAudio2Device::startIO(void)
{
	for (unsigned t = 0; t < 50 && !running_ && thread_on_; t++) {
		IOSleep(10);                                     /* the last run's clock thread is still leaving */
	}
	IOLockLock(lock_);
	if (running_ || thread_on_) {
		IOLockUnlock(lock_);
		return running_ ? kIOReturnSuccess : kIOReturnBusy;
	}
	seed_++;
	t0_ = mach_absolute_time();
	pa_publish(this, 0);
	running_ = true;
	thread_on_ = true;
	thread_t thread;
	if (kernel_thread_start(pa_clock_thread, this, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		running_ = false;
		thread_on_ = false;
	}
	IOLockUnlock(lock_);
	IOLog("PANDORA AUDIO %s: IO started (seed %llu)\n", tag(), seed_);
	return running_ ? kIOReturnSuccess : kIOReturnNoResources;
}

void
IOAudio2Device::stopIO(void)
{
	IOLockLock(lock_);
	const bool was = running_;
	running_ = false;
	IOLockUnlock(lock_);
	if (was) {
		IOLog("PANDORA AUDIO %s: IO stopped\n", tag());
	}
}

IOReturn
IOAudio2DeviceUserClient::clientClose(void)
{
	if (started_ && device_ != nullptr) {
		device_->stopIO();
		started_ = false;
	}
	IOLog("PANDORA AUDIO %s: closed by %s\n", device_ != nullptr ? device_->tag() : "?", name_);
	terminate();
	return kIOReturnSuccess;
}

void
IOAudio2DeviceUserClient::free(void)
{
	OSSafeReleaseNULL(device_);
	IOUserClient::free();
}

IOReturn
IOAudio2DeviceUserClient::setProperties(OSObject *properties)
{
	return device_ != nullptr ? device_->setProperties(properties) : kIOReturnNotReady;
}

IOReturn
IOAudio2DeviceUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	IOLog("PANDORA AUDIO %s: %s: notification port %s, type %u, ref %#llx\n",
	    device_ != nullptr ? device_->tag() : "?", name_, port ? "set" : "cleared", (unsigned)type,
	    (unsigned long long)refCon);
	return kIOReturnSuccess;
}

IOReturn
IOAudio2DeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOMemoryDescriptor *md = nullptr;
	if (device_ != nullptr && (type >> 28) == 0) {
		md = device_->status_;
	} else if (device_ != nullptr && (type >> 28) == 1 && (type & 0x0fffffffu) == PA_STREAM_ID) {
		md = device_->ring_;
	}
	IOLog("PANDORA AUDIO %s: %s maps type %#x (options %#x): %s\n", device_ != nullptr ? device_->tag() : "?",
	    name_, (unsigned)type, options != nullptr ? (unsigned)*options : 0u, md == nullptr ? "refused" :
	    (type >> 28) == 0 ? "engine status" : "IO buffer");
	if (md == nullptr) {
		return kIOReturnBadArgument;
	}
	md->retain();
	*memory = md;
	return kIOReturnSuccess;
}

IOExternalTrap *
IOAudio2DeviceUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	static IOExternalTrap trap = {nullptr, static_cast<IOTrap>(&IOAudio2DeviceUserClient::doIO)};
	if (index != 0) {
		return nullptr;
	}
	*targetP = this;
	return &trap;
}

IOReturn
IOAudio2DeviceUserClient::doIO(void *, void *, void *, void *, void *, void *)
{
	if (device_ != nullptr) {
		device_->traps_++;
	}
	return kIOReturnSuccess;
}

static const char *const pa_method_names[13] = {"engine start", "engine stop", "set control value",
	                                         "config change", "property action", "set stream format",
	                                         "set stream active", "move data exchange block",
	                                         "set multi control value", "move block control data",
	                                         "start IO with flags", "stop IO with flags", "client description"};

IOReturn
IOAudio2DeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (device_ == nullptr || selector >= 13) {
		IOLog("PANDORA AUDIO %s: %s: method %u refused\n", device_ != nullptr ? device_->tag() : "?", name_,
		    selector);
		return kIOReturnUnsupported;
	}
	uint64_t s[4] = {};
	for (uint32_t i = 0; i < a->scalarInputCount && i < 4; i++) {
		s[i] = a->scalarInput[i];
	}
	IOLog("PANDORA AUDIO %s: %s: method %u (%s): %u scalars %#llx %#llx %#llx, struct in %u, out %u scalars %u "
	    "struct\n", device_->tag(), name_, selector, pa_method_names[selector], a->scalarInputCount, s[0], s[1], s[2],
	    a->structureInputSize, a->scalarOutputCount, a->structureOutputSize);
	IOReturn ret = kIOReturnSuccess;
	switch (selector) {
	case 2:                                                  /* set control value: echo the value */
		if (a->scalarOutputCount >= 1) {
			a->scalarOutput[0] = s[1];
		}
		break;
	case 5:                                                  /* stream id + AudioStreamBasicDescription */
		if (a->structureInputSize >= 40 && a->structureInput != nullptr) {
			const uint8_t *b = (const uint8_t *)a->structureInput;
			uint64_t rate_bits;                      /* a double; no floating point in the kernel */
			uint32_t f[8];
			memcpy(&rate_bits, b, 8);
			memcpy(f, b + 8, 32);
			IOLog("PANDORA AUDIO %s: %s: format of stream %llu: rate bits %#llx (48000.0 = 0x40e7700000000000) "
			    "id %#x flags %#x %u B/packet %u frames/packet %u B/frame %u ch %u bits\n", device_->tag(), name_, s[0],
			    rate_bits, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
		}
		break;
	case 6:                                                  /* stream ID, active */
		if (s[1] != 0) {
			device_->routed_ = true;
		}
		break;
	case 8:
		ret = kIOReturnUnsupported;
		break;
	case 0:
	/* 0294: aud20: the default device routed to us, the plug-in sound 1106 starts IO with method 0 (engine start)
	 * and no method 10; HAL waits for the first zero time stamp, gives up after 5 s ("the IO thread failed to start,
	 * Error: 'nope'") and sends method 1. The clock starts with either. */
	case 10:
		ret = device_->startIO();
		started_ = ret == kIOReturnSuccess;
		if (a->scalarOutputCount >= 1) {
			a->scalarOutput[0] = 0;
		}
		break;
	case 1:
	case 11:
		device_->stopIO();
		started_ = false;
		if (a->scalarOutputCount >= 1) {
			a->scalarOutput[0] = 0;
		}
		break;
	default:
		break;
	}
	return ret;
}
#endif /* SWEET */
