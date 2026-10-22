p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = open(p).read()


def sub(o, n):
    global s
    assert s.count(o) == 1, o[:80]
    s = s.replace(o, n)


backlight = r'''#if defined(PANDORA)
/*
 * Backlight service for CoreBrightness (0137). BrightnessControl's
 * BCAppleBacklightBrtControl looks up a backlight service (IOServiceMatching
 * "AppleARMBacklight" / IOPropertyMatch "backlight-control"), reads its
 * "IODisplayParameters" and sets brightness with IORegistryEntrySetCFProperty. Every
 * property it writes is logged; numbers under "brightness", "BrightnessMilliNits" or
 * "brightness-nits" (also inside a dictionary, or a parameter dictionary's "value")
 * become the pixel scale used by present() (sweet_fb_dim).
 */
#include <libkern/c++/OSCollectionIterator.h>
class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void publishParameters(void);
};
OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static unsigned sweet_bl_logs;

static void
sweet_bl_param(OSDictionary *params, const char *key, uint32_t min, uint32_t max, uint32_t value)
{
	OSDictionary *d = OSDictionary::withCapacity(3);
	const char *names[3] = {"min", "max", "value"};
	uint32_t vals[3] = {min, max, value};
	for (int i = 0; d && i < 3; i++) {
		OSNumber *n = OSNumber::withNumber(vals[i], 32);
		if (n) {
			d->setObject(names[i], n);
			n->release();
		}
	}
	if (d) {
		params->setObject(key, d);
		d->release();
	}
}

void
AppleARMBacklight::publishParameters(void)
{
	OSDictionary *params = OSDictionary::withCapacity(2);
	if (params) {
		/* brightness: 0..65536 (1.0 = the level ABL left); nits as 16.16, 2..500. */
		sweet_bl_param(params, "brightness", 0, 65536, 65536);
		sweet_bl_param(params, "brightness-nits", 2 << 16, 500 << 16, 500 << 16);
		setProperty("IODisplayParameters", params);
		params->release();
	}
	setProperty("backlight-control", kOSBooleanTrue);
	setProperty("user-accessible-max-nits", 500ULL, 32);
	setProperty("edr-max-nits", 500ULL, 32);
}

/* Returns the number under key (or the "value" of a parameter dictionary there). */
static bool
sweet_bl_number(OSObject *o, uint64_t *out)
{
	OSNumber *n = OSDynamicCast(OSNumber, o);
	OSDictionary *d = OSDynamicCast(OSDictionary, o);
	if (!n && d) {
		n = OSDynamicCast(OSNumber, d->getObject("value"));
	}
	if (!n) {
		return false;
	}
	*out = n->unsigned64BitValue();
	return true;
}

static void
sweet_bl_apply(const OSSymbol *key, OSObject *o, int depth)
{
	uint64_t v = 0;
	bool num = sweet_bl_number(o, &v);
	if (sweet_bl_logs < 60) {
		sweet_bl_logs++;
		if (num) {
			IOLog("SWEET BL: %s%s = %llu (%#llx)\n", depth ? "  " : "", key->getCStringNoCopy(), v, v);
		} else {
			IOLog("SWEET BL: %s%s = <%s>\n", depth ? "  " : "", key->getCStringNoCopy(),
			    o->getMetaClass()->getClassName());
		}
	}
	uint64_t frac = ~0ULL;      /* 0..1000 */
	if (num && key->isEqualTo("brightness")) {
		frac = v * 1000 / 65536;
	} else if (num && key->isEqualTo("BrightnessMilliNits")) {
		frac = v / 500;
	} else if (num && key->isEqualTo("brightness-nits")) {
		frac = (v >> 16) * 1000 / 500;
	}
	if (frac != ~0ULL) {
		frac = frac > 1000 ? 1000 : frac;
		uint32_t dim = (uint32_t)(frac * 256 / 1000);
		sweet_fb_dim = dim < 16 ? 16 : dim;
		if (sweet_bl_logs < 60) {
			IOLog("SWEET BL: -> scale %u/256\n", sweet_fb_dim);
		}
		return;
	}
	OSDictionary *d = OSDynamicCast(OSDictionary, o);
	if (d && depth == 0) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
		const OSSymbol *k;
		while (it && (k = OSDynamicCast(OSSymbol, it->getNextObject()))) {
			sweet_bl_apply(k, d->getObject(k), 1);
		}
		OSSafeReleaseNULL(it);
	}
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	if (!d) {
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	const OSSymbol *k;
	while (it && (k = OSDynamicCast(OSSymbol, it->getNextObject()))) {
		sweet_bl_apply(k, d->getObject(k), 0);
	}
	OSSafeReleaseNULL(it);
	return kIOReturnSuccess;
}
#endif

bool
AppleCLCD::start(IOService *provider)
{'''

sub("""bool
AppleCLCD::start(IOService *provider)
{""", backlight)

sub("""	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\\n",""",
    """	registerService();
#if defined(PANDORA)
	{
		AppleARMBacklight *bl = OSTypeAlloc(AppleARMBacklight);
		if (bl && bl->init() && bl->attach(this)) {
			bl->publishParameters();
			if (bl->start(this)) {
				bl->registerService();
				IOLog("SWEET BL: AppleARMBacklight published\\n");
			} else {
				bl->detach(this);
			}
		}
		OSSafeReleaseNULL(bl);
	}
#endif
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\\n",""")
open(p, 'w').write(s)
print('patched')
