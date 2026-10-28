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
pa_stream(void)
{
	OSDictionary *s = OSDictionary::withCapacity(6);
	OSDictionary *cur = pa_format(false), *avail = pa_format(true);
	OSArray *list = avail == nullptr ? nullptr : OSArray::withObjects((const OSObject **)&avail, 1);
	if (s == nullptr || cur == nullptr || list == nullptr) {
		OSSafeReleaseNULL(s);
	} else {
		pa_num(s, "stream ID", PA_STREAM_ID, 32);
		pa_num(s, "starting channel", 1, 32);
		pa_num(s, "terminal type", PA_4CC('s', 'p', 'k', 'r'), 32);   /* kAudioStreamTerminalTypeSpeaker */
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
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	IOReturn doIO(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
};

OSDefineMetaClassAndStructors(IOAudio2DeviceUserClient, IOUserClient);

/* pandora_audio_delay (0288): publish the device once the uptime reaches the boot-arg's seconds. */
static void
pa_publish_later(void *arg, wait_result_t)
{
	IOAudio2Device *d = static_cast<IOAudio2Device *>(arg);
	int delay = 0;
	(void)PE_parse_boot_argn("pandora_audio_delay", &delay, sizeof(delay));
	uint64_t ns = 0;
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		if (ns >= (uint64_t)delay * NSEC_PER_SEC) {
			break;
		}
		IOSleep(200);
	}
	d->registerService();
	IOLog("PANDORA AUDIO: IOAudio2Device registered at %llu ms\n", ns / NSEC_PER_MSEC);
	d->release();
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
	OSDictionary *stream = pa_stream();
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
	/* 0282: the UID VirtualAudio looks the speaker up by (aud1: HAL activated "PandoraBuiltInSpeaker" as device 33,
	 * but VirtualAudio built no route to it). Its table of physical devices takes the iPhone's UIDs: "Codec"
	 * (CS42L75: receiver, headphones, mics), "Speaker" (CS35L26), "Voice", "Reference", "Baseband Voice". */
	setProperty("device UID", "Speaker");
	setProperty("device name", "Speaker");
	setProperty("device manufacturer", "Pandora");
	/* 0283: boot-arg pandora_audio_tt=1 makes it a Thunderbolt device, as sheng's plug-in on iPadOS 17
	 * (VirtualAudio there routes a Thunderbolt device as a generic driver device; built-in devices it takes only
	 * from the product's transducer database). aud2/aud3 (built-in, UID "Speaker"): still no route. */
	int tt = 0;
	(void)PE_parse_boot_argn("pandora_audio_tt", &tt, sizeof(tt));
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
	/*
	 * 0288: boot-arg pandora_audio_delay=SECONDS publishes the device that many seconds after boot. aud9: VirtualAudio
	 * routes the default device the moment the speaker appears ("Culling route connection because ringer switch is
	 * off", pap -> pspk) and keeps the null device after SpringBoard turns the ringer on 0.3 s later; system sounds
	 * then run on Null_Device. Published after SpringBoard has set the ringer state, the speaker is routed.
	 */
	int delay = 0;
	(void)PE_parse_boot_argn("pandora_audio_delay", &delay, sizeof(delay));
	if (delay > 0) {
		retain();
		thread_t thread;
		if (kernel_thread_start(pa_publish_later, this, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			release();
			registerService();
		}
	} else {
		registerService();
	}
	IOLog("PANDORA AUDIO: IOAudio2Device %s: 1 output stream, %u Hz, %u channels, s32, ring %u frames\n",
	    delay > 0 ? "made, published later" : "registered", PA_RATE, PA_CHANNELS, PA_RING_FRAMES);
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
		IOLog("PANDORA AUDIO: %s sets \"%s\" = %s%llu%s\n", name, k->getCStringNoCopy(),
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
	uc->device_ = this;
	proc_selfname(uc->name_, sizeof(uc->name_));
	if (!uc->attach(this) || !uc->start(this)) {
		uc->detach(this);
		uc->release();
		return kIOReturnError;
	}
	IOLog("PANDORA AUDIO: opened by %s (type %u)\n", uc->name_, (unsigned)type);
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
			IOLog("PANDORA AUDIO: running: wrap %llu, ring peak %#x, %u of %u samples nonzero, traps %u\n", n,
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
	IOLog("PANDORA AUDIO: IO started (seed %llu)\n", seed_);
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
		IOLog("PANDORA AUDIO: IO stopped\n");
	}
}

IOReturn
IOAudio2DeviceUserClient::clientClose(void)
{
	if (started_ && device_ != nullptr) {
		device_->stopIO();
		started_ = false;
	}
	IOLog("PANDORA AUDIO: closed by %s\n", name_);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOAudio2DeviceUserClient::setProperties(OSObject *properties)
{
	return device_ != nullptr ? device_->setProperties(properties) : kIOReturnNotReady;
}

IOReturn
IOAudio2DeviceUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	IOLog("PANDORA AUDIO: %s: notification port %s, type %u, ref %#llx\n", name_, port ? "set" : "cleared",
	    (unsigned)type, (unsigned long long)refCon);
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
	IOLog("PANDORA AUDIO: %s maps type %#x (options %#x): %s\n", name_, (unsigned)type,
	    options != nullptr ? (unsigned)*options : 0u, md == nullptr ? "refused" : (type >> 28) == 0 ?
	    "engine status" : "IO buffer");
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
		IOLog("PANDORA AUDIO: %s: method %u refused\n", name_, selector);
		return kIOReturnUnsupported;
	}
	uint64_t s[4] = {};
	for (uint32_t i = 0; i < a->scalarInputCount && i < 4; i++) {
		s[i] = a->scalarInput[i];
	}
	IOLog("PANDORA AUDIO: %s: method %u (%s): %u scalars %#llx %#llx %#llx, struct in %u, out %u scalars %u struct\n",
	    name_, selector, pa_method_names[selector], a->scalarInputCount, s[0], s[1], s[2], a->structureInputSize,
	    a->scalarOutputCount, a->structureOutputSize);
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
			IOLog("PANDORA AUDIO: %s: format of stream %llu: rate bits %#llx (48000.0 = 0x40e7700000000000) id %#x "
			    "flags %#x %u B/packet %u frames/packet %u B/frame %u ch %u bits\n", name_, s[0], rate_bits, f[0],
			    f[1], f[2], f[3], f[4], f[5], f[6]);
		}
		break;
	case 8:
		ret = kIOReturnUnsupported;
		break;
	case 10:
		ret = device_->startIO();
		started_ = ret == kIOReturnSuccess;
		if (a->scalarOutputCount >= 1) {
			a->scalarOutput[0] = 0;
		}
		break;
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
