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
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
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
#define PA_CODEC_MICS     2u                                 /* 0303: the microphones the Codec device carries */
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
pa_format(bool range, uint32_t channels = PA_CHANNELS)
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
	pa_num(f, "bytes per packet", channels * 4u, 32);
	pa_num(f, "frames per packet", 1, 32);
	pa_num(f, "bytes per frame", channels * 4u, 32);
	pa_num(f, "channels per frame", channels, 32);
	pa_num(f, "bits per channel", 32, 32);
	return f;
}

static OSDictionary *
pa_stream(bool input, uint32_t id = PA_STREAM_ID, uint32_t terminal = 0, uint32_t channels = PA_CHANNELS,
    uint32_t max_channels = 0)
{
	OSDictionary *s = OSDictionary::withCapacity(6);
	OSDictionary *cur = pa_format(false, channels), *avail = pa_format(true, channels);
	OSArray *list = avail == nullptr ? nullptr : OSArray::withObjects((const OSObject **)&avail, 1);
	for (uint32_t ch = 1; list != nullptr && ch <= max_channels; ch++) {   /* 0303: the other formats offered */
		if (ch != channels) {
			OSDictionary *f = pa_format(true, ch);
			if (f != nullptr) {
				list->setObject(f);
			}
			OSSafeReleaseNULL(f);
		}
	}
	if (s == nullptr || cur == nullptr || list == nullptr) {
		OSSafeReleaseNULL(s);
	} else {
		pa_num(s, "stream ID", id, 32);
		pa_num(s, "starting channel", 1, 32);
		/* kAudioStreamTerminalTypeMicrophone / kAudioStreamTerminalTypeSpeaker unless given */
		pa_num(s, "terminal type", terminal ? terminal : input ? PA_4CC('m', 'i', 'c', 'r') : PA_4CC('s', 'p', 'k', 'r'),
		    32);
		s->setObject("current format", cur);
		s->setObject("available formats", list);
	}
	OSSafeReleaseNULL(cur);
	OSSafeReleaseNULL(avail);
	OSSafeReleaseNULL(list);
	return s;
}

/*
 * 0299: a control as IOAudio2Family describes one (keys in the iPhone X kernelcache next to "control ID": "base class",
 * "class", "scope", "element", "read only", "value", "selectors"; CoreAudio's HALS_IOA2UCDevice reads the same).
 * ITEMS: for a selector, its items {"value", "name"}.
 */
static OSDictionary *
pa_control(uint32_t id, uint32_t base, uint32_t cls, uint32_t scope, uint32_t element, uint32_t value,
    const uint32_t *items = nullptr, const char *const *names = nullptr, unsigned nitems = 0)
{
	OSDictionary *c = OSDictionary::withCapacity(8);
	if (c == nullptr) {
		return nullptr;
	}
	pa_num(c, "control ID", id, 32);
	pa_num(c, "base class", base, 32);
	pa_num(c, "class", cls, 32);
	pa_num(c, "scope", scope, 32);
	pa_num(c, "element", element, 32);
	pa_num(c, "value", value, 32);
	c->setObject("read only", kOSBooleanFalse);
	if (nitems) {
		OSArray *list = OSArray::withCapacity(nitems);
		for (unsigned i = 0; list != nullptr && i < nitems; i++) {
			OSDictionary *it = OSDictionary::withCapacity(2);
			OSString *name = OSString::withCString(names[i]);
			if (it != nullptr && name != nullptr) {
				pa_num(it, "value", items[i], 32);
				it->setObject("name", name);
				list->setObject(it);
			}
			OSSafeReleaseNULL(it);
			OSSafeReleaseNULL(name);
		}
		if (list != nullptr) {
			c->setObject("selectors", list);
		}
		OSSafeReleaseNULL(list);
	}
	return c;
}

class IOAudio2Device : public IOService {
	OSDeclareDefaultStructors(IOAudio2Device);

public:
	IOBufferMemoryDescriptor *status_ = nullptr;
	IOBufferMemoryDescriptor *ring_ = nullptr;
	IOBufferMemoryDescriptor *in_ring_ = nullptr;           /* 0299: the "Codec" device's input stream (ID 2) */
	IOLock *lock_ = nullptr;
	volatile bool running_ = false;
	volatile bool thread_on_ = false;
	uint64_t seed_ = 0;
	uint64_t t0_ = 0;
	volatile uint32_t traps_ = 0;
	bool hawking_ = false;                                   /* 0292: the "Hawking" input device, else "Speaker" */
	bool codec_ = false;                                     /* 0299: the "Codec" device (receiver, microphones) */
	volatile bool out_active_ = false;                       /* 0300: its output stream set active (method 6) */
	uint32_t in_channels_ = 1;                               /* 0303: the Codec's microphone stream's channels */
	volatile bool routed_ = false;                           /* 0293: a client set a stream active */
	uint64_t rp_ = 0, rp_seed_ = 0;                          /* 0295: where pandora_audio_out_fill reads */
	uint32_t resyncs_ = 0;
	uint64_t lv_seed_ = 0, lv_sq_[2] = {};                   /* 0305: the start's levels (pandora_audio_out_fill) */
	uint32_t lv_frames_ = 0, lv_win_ = 0, lv_peak_[2] = {};

	const char *tag(void) const { return codec_ ? "Codec" : hawking_ ? "Hawking" : "Speaker"; }
	bool speaker(void) const { return !hawking_ && !codec_; }
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
static IOAudio2Device *pa_codec;                             /* 0300: the "Codec" device */
static IOAudio2Device *volatile pa_out;                      /* 0300: the device the DSP plays (pa_route_update) */

extern "C" void pandora_audio_ringer_known(void);
void
pandora_audio_ringer_known(void)
{
	pa_ringer_known = true;
}

/* 0298: the amps' power follows the speaker's IO (bsd/dev/pandora_amp.c); a power off or restart takes them down
 * first, through the root domain's priority power-state message (IOPMrootDomain::handlePlatformHaltRestart sends
 * kIOMessageSystemWillPowerOff / WillRestart to these clients one after another and waits for each). */
#define PA_AMP_WAIT_MS 300u
extern "C" int pandora_amp_io(int mode, unsigned wait_ms);
extern "C" void pandora_amp_final(const char *why);
extern "C" int pandora_adsp_route(unsigned which, unsigned wait_ms);

/*
 * 0300: the route: the receiver while the "Codec" device runs with its output stream active (VirtualAudio sets the
 * stream of the port it routes to active, method 6), else the speaker while the "Speaker" device runs, else nothing.
 * A change goes as on Android: the amps down, the DSP's graph closed and the route's one opened (pandora_adsp.c;
 * PRM's TDM clock with it), the amps up in the route's mode (pandora_amp.c: speaker, or the top amp alone on the
 * earpiece). Called before a start's clock starts (STARTING runs from then on), after a stop and when a stream is set
 * active; the caller waits for it, as a start waits for the amps since 0298.
 */
static IOLock *pa_route_lock;
static int pa_route_mode;                                    /* 0 none, 1 speaker, 2 receiver */
static const char *const pa_route_name[3] = {"none", "speaker", "receiver"};

static void
pa_route_update(IOAudio2Device *starting)
{
	if (pa_route_lock == nullptr) {
		return;
	}
	IOLockLock(pa_route_lock);
	IOAudio2Device *spk = pa_speaker, *codec = pa_codec;
	const bool codec_on = codec != nullptr && (codec->running_ || codec == starting) && codec->out_active_;
	const bool spk_on = spk != nullptr && (spk->running_ || spk == starting);
	const int want = codec_on ? 2 : spk_on ? 1 : 0;
	if (want != pa_route_mode) {
		const uint64_t t0 = mach_absolute_time();
		const int down = pa_route_mode ? pandora_amp_io(0, PA_AMP_WAIT_MS) : 0;
		pa_out = want == 2 ? codec : want == 1 ? spk : nullptr;
		const int graph = pandora_adsp_route((unsigned)want, 1000);
		const int up = want ? pandora_amp_io(want, PA_AMP_WAIT_MS) : 0;
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		IOLog("PANDORA AUDIO: route %s -> %s in %llu ms (amps down %d, graph %d, amps up %d)\n",
		    pa_route_name[pa_route_mode], pa_route_name[want], ns / NSEC_PER_MSEC, down, graph, up);
		pa_route_mode = want;
	}
	IOLockUnlock(pa_route_lock);
}

static IOReturn
pa_power_message(void *, void *, UInt32 type, IOService *, void *, vm_size_t)
{
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) {
		pandora_amp_final(type == kIOMessageSystemWillRestart ? "restart" : "power off");
	}
	return kIOReturnSuccess;
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
	if (pa_route_lock == nullptr) {
		pa_route_lock = IOLockAlloc();                       /* 0300; devices start one after another */
	}
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
	/*
	 * 0299: a third instance (personality PandoraAudioRole = Codec) is the iPhone's "Codec" device (iPhone X:
	 * AppleCS42L75Audio's "Codec": receiver, headphones and microphones). VirtualAudio's Device_Codec_Aspen
	 * (0x23f8d0) makes its ports from the device's properties: a receiver port ("Built-In Receiver") when
	 * kAudioDevicePropertyMute exists on output element 3 (element 4 makes a speaker port, 5 hearing aid
	 * compliance), an internal microphone port when kAudioDevicePropertyDataSources ('ssc#') on the input scope lists
	 * one of imic, smic, fmic, lmic, tmic, umic, imi1-imi4 and the transducer database has imic or imi1. In IOAudio2
	 * terms: a mute control on output element 3 and a data source selector on the input. Output stream 1 (the
	 * receiver), input stream 2 (the microphones, silent until they are driven). Published at once, as Hawking.
	 * 0303: aud31: every route with the built-in microphone failed to activate: its DSP chain "d22_bottom_mic_general"
	 * has an AUAGCV2 that refused the 2-channel format ("DSP (agc2) format setup failed with error: (-10868) ...
	 * [ 32/48000/2 ...]", "Routing is not supported"), so play-and-record fell back to the speaker. The device now
	 * describes its channels as iPhone X's AppleEmbeddedAudio does (kernelcache 20H392, 0xfffffff00597c3a0):
	 * "maxChannelCount" {"MaxOutputChannelCount", "MaxInputChannelCount": the hardware's channels},
	 * "LowPowerEnableChannelMask" and "LowPowerActiveChannelMask" 0, and "custom property info", the table HAL reads
	 * its custom properties through ({"property selector", "registry key"}: 'MCCs' maxChannelCount, 'LPEm', 'LPAm');
	 * VirtualAudio's Device_Codec_Aspen reads MaxInputChannelCount from 'MCCs'. The input carries this phone's
	 * microphones: two, as Android's handset recording (handset-dmic-endfire: AMIC1 bottom and AMIC4).
	 * aud32: the same refusal with two channels; HAL never asked for another format (method 5), as only one was
	 * offered. The stream now offers mono (current: one microphone, what iPhone X's general recording chain takes)
	 * and two channels; method 5 switches between them.
	 */
	codec_ = role != nullptr && role->isEqualTo("Codec");
	if (codec_) {
		static const uint32_t mics[5] = {PA_4CC('i', 'm', 'i', '1'), PA_4CC('i', 'm', 'i', '2'),
		    PA_4CC('i', 'm', 'i', '3'), PA_4CC('i', 'm', 'i', '4'), PA_4CC('i', 'm', 'i', 'c')};
		static const char *const mic_names[5] = {"Bottom", "Front", "Back", "Top", "Microphone"};
		in_ring_ = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, PA_RING_BYTES,
		    page_size);
		OSSafeReleaseNULL(stream);
		stream = pa_stream(false, 1, PA_4CC('r', 's', 'p', 'k'));     /* kAudioStreamTerminalTypeReceiverSpeaker */
		OSDictionary *in = pa_stream(true, 2, 0, in_channels_, PA_CODEC_MICS);
		OSDictionary *mute = pa_control(1, PA_4CC('t', 'o', 'g', 'l'), PA_4CC('m', 'u', 't', 'e'),
		    PA_4CC('o', 'u', 't', 'p'), 3, 0);
		OSDictionary *source = pa_control(2, PA_4CC('s', 'l', 'c', 't'), PA_4CC('d', 's', 'r', 'c'),
		    PA_4CC('i', 'n', 'p', 't'), 0, mics[0], mics, mic_names, 5);
		if (in_ring_ == nullptr || stream == nullptr || in == nullptr || mute == nullptr || source == nullptr) {
			IOLog("PANDORA AUDIO Codec: no memory for the device\n");
			OSSafeReleaseNULL(stream);
			OSSafeReleaseNULL(in);
			OSSafeReleaseNULL(mute);
			OSSafeReleaseNULL(source);
			OSSafeReleaseNULL(outs);
			OSSafeReleaseNULL(ins);
			OSSafeReleaseNULL(controls);
			return false;
		}
		bzero(in_ring_->getBytesNoCopy(), PA_RING_BYTES);
		outs->replaceObject(0, stream);
		ins->setObject(in);
		controls->setObject(mute);
		controls->setObject(source);
		setProperty("device UID", "Codec");
		setProperty("device name", "Codec");
		setProperty("device manufacturer", "Pandora");
		setProperty("transport type", PA_4CC('b', 'l', 't', 'n'), 32);
		setProperty("sample rate", (unsigned long long)PA_RATE << 32, 64);
		setProperty("io buffer frame size", PA_RING_FRAMES, 32);
		setProperty("output latency", PA_OUT_LATENCY, 32);
		setProperty("output safety offset", PA_OUT_SAFETY, 32);
		setProperty("input latency", PA_OUT_LATENCY, 32);
		setProperty("input safety offset", PA_OUT_SAFETY, 32);
		setProperty("clock domain", PA_4CC('p', 'a', 'n', 'd'), 32);
		setProperty("output streams", outs);
		setProperty("input streams", ins);
		setProperty("controls", controls);
		{
			OSDictionary *maxc = OSDictionary::withCapacity(2);
			OSArray *info = OSArray::withCapacity(3);
			static const struct {
				uint32_t selector;
				const char *key;
			} custom[3] = {{PA_4CC('M', 'C', 'C', 's'), "maxChannelCount"},
			    {PA_4CC('L', 'P', 'E', 'm'), "LowPowerEnableChannelMask"},
			    {PA_4CC('L', 'P', 'A', 'm'), "LowPowerActiveChannelMask"}};
			if (maxc != nullptr && info != nullptr) {
				pa_num(maxc, "MaxOutputChannelCount", PA_CHANNELS, 32);
				pa_num(maxc, "MaxInputChannelCount", PA_CODEC_MICS, 32);
				setProperty("maxChannelCount", maxc);
				setProperty("LowPowerEnableChannelMask", 0ull, 32);
				setProperty("LowPowerActiveChannelMask", 0ull, 32);
				for (unsigned i = 0; i < 3; i++) {
					OSDictionary *e = OSDictionary::withCapacity(2);
					OSString *k = OSString::withCString(custom[i].key);
					if (e != nullptr && k != nullptr) {
						pa_num(e, "property selector", custom[i].selector, 32);
						e->setObject("registry key", k);
						info->setObject(e);
					}
					OSSafeReleaseNULL(e);
					OSSafeReleaseNULL(k);
				}
				setProperty("custom property info", info);
			}
			OSSafeReleaseNULL(maxc);
			OSSafeReleaseNULL(info);
		}
		OSSafeReleaseNULL(stream);
		OSSafeReleaseNULL(in);
		OSSafeReleaseNULL(mute);
		OSSafeReleaseNULL(source);
		OSSafeReleaseNULL(outs);
		OSSafeReleaseNULL(ins);
		OSSafeReleaseNULL(controls);
		pa_codec = this;
		registerService();
		IOLog("PANDORA AUDIO Codec: registered: receiver output (mute control, element 3), microphone input "
		    "(data source selector), %u Hz, %u channels out, %u in, s32\n", PA_RATE, PA_CHANNELS, PA_CODEC_MICS);
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
		if (registerPrioritySleepWakeInterest(pa_power_message, this, nullptr) == nullptr) {
			IOLog("PANDORA AUDIO Speaker: no power-off message: the amps are not taken down first\n");
		}
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
	OSSafeReleaseNULL(in_ring_);
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
			IOLog("PANDORA AUDIO %s: running: wrap %llu, ring peak %#x, %u of %u samples nonzero, traps %u, DSP reads at "
			    "%llu (%u restarts)\n", tag(), n, peak, nonzero, PA_RING_FRAMES * PA_CHANNELS, traps_, rp_, resyncs_);
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
	/* 0298: the amps up before the clock starts (Android powers them up before its first buffer, iOS before the
	 * engine runs): the start waits for them, up to PA_AMP_WAIT_MS, and goes on either way. 0300: the route's graph
	 * and amps (pa_route_update). */
	if (!hawking_ && !running_) {
		pa_route_update(this);
	}
	for (unsigned t = 0; t < 50 && !running_ && thread_on_; t++) {
		IOSleep(10);                                     /* the last run's clock thread is still leaving */
	}
	IOLockLock(lock_);
	if (running_ || thread_on_) {
		IOLockUnlock(lock_);
		return running_ ? kIOReturnSuccess : kIOReturnBusy;
	}
	seed_++;
	bzero(ring_->getBytesNoCopy(), PA_RING_BYTES);           /* 0295: no tail of the last run reaches the DSP */
	if (in_ring_ != nullptr) {
		bzero(in_ring_->getBytesNoCopy(), PA_RING_BYTES);    /* 0299: the microphones are silent for now */
	}
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
		if (!hawking_) {
			pa_route_update(nullptr);          /* 0298/0300: the amps down and the graph closed, as Android's standby */
		}
	}
}

/*
 * 0295: the speaker's samples for the audio DSP (pandora_adsp.c, pandora_spk_live=1): FRAMES interleaved s32 stereo
 * frames, slot 0 the top amp, slot 1 the bottom, as the ring holds them. Silence when IO is not running. The DSP takes
 * a 20 ms buffer each time one comes back and keeps four queued; reads follow on from the last one and stay behind
 * the sample time of now (our clock, the one the zero time stamps come from), where HAL has written already: it
 * writes the safety offset (960 frames) ahead of now. A run starts PA_OUT_LAG + 4 buffers back so that after the
 * first four the reads are PA_OUT_LAG behind; a read ahead of now or more than PA_OUT_SLACK behind starts over there.
 * Returns the frames taken from the ring.
 */
#define PA_OUT_LAG        960u
#define PA_OUT_SLACK      (PA_RING_FRAMES / 2u)

extern "C" uint32_t pandora_audio_out_fill(int32_t *dst, uint32_t frames);
uint32_t
pandora_audio_out_fill(int32_t *dst, uint32_t frames)
{
	IOAudio2Device *d = pa_out;                               /* 0300: the route's device */
	if (d == nullptr || !d->running_ || d->ring_ == nullptr || frames > PA_RING_FRAMES) {
		bzero(dst, (size_t)frames * PA_BYTES_FRAME);
		return 0;
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - d->t0_, &ns);
	const uint64_t now = ns * 3u / 62500u;                    /* frames at 48 kHz */
	const uint64_t start_back = PA_OUT_LAG + 4u * frames;
	if (d->rp_seed_ != d->seed_ || d->rp_ + frames > now || d->rp_ + PA_OUT_SLACK < now) {
		if (d->rp_seed_ == d->seed_) {
			d->resyncs_++;
		}
		d->rp_seed_ = d->seed_;
		d->rp_ = now > start_back ? now - start_back : 0;
	}
	const int32_t *ring = (const int32_t *)d->ring_->getBytesNoCopy();
	const uint32_t at = (uint32_t)(d->rp_ % PA_RING_FRAMES);
	const uint32_t first = frames < PA_RING_FRAMES - at ? frames : PA_RING_FRAMES - at;
	memcpy(dst, ring + (size_t)at * PA_CHANNELS, (size_t)first * PA_BYTES_FRAME);
	if (first < frames) {
		memcpy(dst + (size_t)first * PA_CHANNELS, ring, (size_t)(frames - first) * PA_BYTES_FRAME);
	}
	d->rp_ += frames;
	/*
	 * 0305: the user hears the speaker's sound start low and reach its level after a few seconds (aud31, aud33), and
	 * iOS's samples in the ring rise by about 9 dB over 4 s where the file's peaks rise by 1.4 dB. Per channel, every
	 * 100 ms for 6 s after each start: the peak and the RMS (16-bit scale) of what the DSP gets, to compare with the
	 * file's envelope and between the channels (AURotationShading in iPhone X's speaker chain sets their balance).
	 */
	if (d->lv_seed_ != d->seed_) {
		d->lv_seed_ = d->seed_;
		d->lv_frames_ = d->lv_win_ = 0;
		d->lv_peak_[0] = d->lv_peak_[1] = 0;
		d->lv_sq_[0] = d->lv_sq_[1] = 0;
	}
	for (uint32_t f = 0; d->lv_win_ < 60 && f < frames; f++) {
		for (unsigned c = 0; c < 2; c++) {
			const int32_t v = dst[2 * f + c];
			const uint32_t a = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
			const uint64_t h = a >> 16;
			d->lv_peak_[c] = a > d->lv_peak_[c] ? a : d->lv_peak_[c];
			d->lv_sq_[c] += h * h;
		}
		if (++d->lv_frames_ == PA_RATE / 10u) {
			uint32_t rms[2];
			for (unsigned c = 0; c < 2; c++) {
				const uint64_t m = d->lv_sq_[c] / (PA_RATE / 10u);
				uint32_t r = 0;
				while ((uint64_t)(r + 1) * (r + 1) <= m) {
					r++;
				}
				rms[c] = r;
			}
			d->lv_win_++;
			IOLog("PANDORA AUDIO %s: out +%u ms: L peak %#x rms %u, R peak %#x rms %u\n", d->tag(), d->lv_win_ * 100u,
			    d->lv_peak_[0], rms[0], d->lv_peak_[1], rms[1]);
			d->lv_frames_ = 0;
			d->lv_peak_[0] = d->lv_peak_[1] = 0;
			d->lv_sq_[0] = d->lv_sq_[1] = 0;
		}
	}
	/*
	 * 0306: the speaker's channels as iPhone X's: channel 0 for the bottom (main) speaker, channel 1 for the top one
	 * (on iPhone X the receiver doubles as it), which iOS's speaker chain shades by orientation (AURotationShading).
	 * aud35: iOS's channel 0 carried the full signal from the start, channel 1 started 8-12 dB lower and rose by about
	 * 7 dB over 5 s; with channel 0 on TDM slot 0 (the top amp, Android's left) the bottom speaker played the shaded
	 * channel, heard as a slow start. The DSP's slot 0 drives the top amp, slot 1 the bottom: the two swap here.
	 */
	if (d->speaker()) {
		for (uint32_t f = 0; f < frames; f++) {
			const int32_t t = dst[2 * f];
			dst[2 * f] = dst[2 * f + 1];
			dst[2 * f + 1] = t;
		}
	}
	return frames;
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
	} else if (device_ != nullptr && (type >> 28) == 1 && (type & 0x0fffffffu) == 2 && device_->in_ring_ != nullptr) {
		md = device_->in_ring_;
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
			/* 0303: the Codec's microphone stream takes 1 or 2 channels; its "current format" follows */
			if (device_->codec_ && s[0] == 2 && f[5] >= 1 && f[5] <= PA_CODEC_MICS && f[5] != device_->in_channels_) {
				OSDictionary *in = pa_stream(true, 2, 0, f[5], PA_CODEC_MICS);
				OSArray *ins = in == nullptr ? nullptr : OSArray::withObjects((const OSObject **)&in, 1);
				if (ins != nullptr) {
					device_->setProperty("input streams", ins);
					device_->in_channels_ = f[5];
					IOLog("PANDORA AUDIO Codec: microphone stream now %u channels\n", f[5]);
				} else {
					ret = kIOReturnNoMemory;
				}
				OSSafeReleaseNULL(in);
				OSSafeReleaseNULL(ins);
			}
		}
		break;
	case 6:                                                  /* stream ID, active */
		if (s[1] != 0) {
			device_->routed_ = true;
		}
		if (s[0] == PA_STREAM_ID && !device_->hawking_) {
			const bool was = device_->out_active_;
			device_->out_active_ = s[1] != 0;
			if (was != device_->out_active_ && device_->running_) {
				pa_route_update(nullptr);                    /* 0300: VirtualAudio moved the output while running */
			}
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
