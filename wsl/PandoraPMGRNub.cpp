/*
 * PANDORA (patch 0148): publish an ApplePMGRNub for Apple's original AMFI, prelinked from
 * the iPhone X iOS 16.7.16 kernelcache (tools/mkprelink.py, after UniXNU's NativePrelink).
 *
 * AMFI's developer-mode latch thread waits up to 30 s for a service of class ApplePMGRNub
 * and panics ("AMFI: No PMGR?") without one. It then reads the PMGR always-on register
 * through the nub, ORs in 4 and writes the value back. The nub's read and write forward to
 * the provider stored at nub+0x88 (vtable slots 312/313); with no provider the original
 * code returns 0xffffffff and does nothing on write, which is also what the generic
 * ApplePMGR class does (20H392 ApplePMGR.kext, read 0xfffffff005685734, write 0xfffffff005683dbc).
 * The nub's own overrides (attachToChild, message, compareName, matchLocation) are safe
 * without a provider.
 *
 * So the nub here is the original class from the prelinked ApplePMGR kext, made by its
 * metaclass, with no provider and no hardware behind it. Nothing happens when the kernel
 * has no prelinked ApplePMGR (the class never appears). Boot-arg pandora_pmgr=0 turns it off.
 */
#include <pexpert/arm64/board_config.h>

#if defined(PANDORA)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>

#define PMGR_NUB_SIZE      0x740
#define PMGR_NUB_PROVIDER  0x88

extern "C" void pandora_pmgr_init(void);

static const OSMetaClass *
pandora_pmgr_meta(void)
{
	const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
	const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : NULL;
	OSSafeReleaseNULL(name);
	return meta;
}

/* pandora_pmgr=2: panic with the reason when the nub cannot be published (bring-up);
 * 3: also panic right after a successful publication. */
static int pandora_pmgr_mode = 1;

/*
 * IOAESAccelerator (patch 0155). The original AppleImage4's start waits without a timeout
 * for an IOAESAccelerator and checks its class; only then does its nonce manager open the
 * gate that launchd's ignition sysctl waits on. On the way it asks the accelerator to
 * encrypt 16 bytes (vtable slot 170, performAES with memory descriptors, AES-128 with a
 * hardware key) and panics "performAES: %#x" on any error. Apple's AES engine does not
 * exist here, so both performAES entries do software AES-CBC (as UniXNU's NativeAES does),
 * with a fixed per-device stand-in for hardware keys (below). The object is the original class from the prelinked
 * IOCryptoAcceleratorFamily (20H392: instance 0x120 bytes, vtable 181 entries; 168
 * getAlgorithmName = "AES-CBC", 169/170 performAES), made by its metaclass; only a private
 * copy of its vtable has the two performAES entries replaced, so getMetaClass and every
 * other method stay original.
 */
#define AES_SIZE            0x120
#define AES_VTABLE_SLOTS    181
#define AES_SLOT_NAME       168
#define AES_SLOT_PERFORM    169
#define AES_SLOT_PERFORM_MD 170

static uintptr_t pandora_aes_vtable[2 + AES_VTABLE_SLOTS];
static volatile uint32_t pandora_aes_calls;

/* The original iOS ABI (UniXNU NativeAES, OriginalAESABI.h; 20H392 AppleImage4 fills
 * bits = 128 and a hardware-key selector at offset 36). */
struct pandora_aes_key {
	uint32_t bits;
	uint8_t bytes[32];
	uint32_t selector;
};
enum { PANDORA_AES_ENCRYPT = 0, PANDORA_AES_DECRYPT = 1 };

/*
 * AppleImage4's nonce manager encrypts 16 bytes with a hardware key (UID/GID class,
 * named by selector) and panics on an error. Those keys live inside Apple silicon; here a
 * fixed per-device stand-in is used: SHA-256("PANDORA hardware key" | selector | ECID).
 * Results are stable on this phone and unlike any real iPhone's; nothing is verified
 * against Apple with them. A software key (selector 0) is used as given.
 */
extern "C" uint64_t pandora_aes_ecid;
uint64_t pandora_aes_ecid = 0x50414e444f524101ULL;	/* tools/mkadt_pandora.py PANDORA_ECID */

static int
pandora_aes_cbc(uint8_t *buf, size_t len, uint8_t iv[16], const struct pandora_aes_key *key, unsigned op)
{
	uint8_t raw[32];
	unsigned klen = key->bits / 8;
	if ((key->bits != 128 && key->bits != 192 && key->bits != 256) || !len || (len & 15)) {
		return -1;
	}
	if (key->selector) {
		SHA256_CTX sha;
		SHA256_Init(&sha);
		SHA256_Update(&sha, "PANDORA hardware key", 20);
		SHA256_Update(&sha, &key->selector, sizeof(key->selector));
		SHA256_Update(&sha, &pandora_aes_ecid, sizeof(pandora_aes_ecid));
		SHA256_Final(raw, &sha);
	} else {
		memcpy(raw, key->bytes, klen);
	}
	union {
		aes_encrypt_ctx enc;
		aes_decrypt_ctx dec;
	} ctx;
	uint8_t next[16];
	int r;
	if (op == PANDORA_AES_DECRYPT) {
		memcpy(next, buf + len - 16, 16);
		r = aes_decrypt_key(raw, (int)klen, &ctx.dec);
		if (!r) {
			r = aes_decrypt_cbc(buf, iv, (unsigned)(len / 16), buf, &ctx.dec);
		}
	} else {
		r = aes_encrypt_key(raw, (int)klen, &ctx.enc);
		if (!r) {
			r = aes_encrypt_cbc(buf, iv, (unsigned)(len / 16), buf, &ctx.enc);
		}
		if (!r) {
			memcpy(next, buf + len - 16, 16);
		}
	}
	if (!r) {
		memcpy(iv, next, 16);
	}
	bzero(&ctx, sizeof(ctx));
	bzero(raw, sizeof(raw));
	return r;
}

static IOReturn
pandora_aes_perform(void *self, void *in, void *out, task_t task, unsigned long long bytes,
    void *iv, unsigned op, void *key, void (*done)(void *, int), void *ref)
{
	(void)self;
	pandora_aes_calls++;
	if (task != kernel_task || !in || !out || !iv || !key || !bytes || bytes > 65536 ||
	    (op != PANDORA_AES_ENCRYPT && op != PANDORA_AES_DECRYPT)) {
		return kIOReturnBadArgument;
	}
	if (in != out) {
		memmove(out, in, (size_t)bytes);
	}
	IOReturn r = pandora_aes_cbc((uint8_t *)out, (size_t)bytes, (uint8_t *)iv,
	    (const struct pandora_aes_key *)key, op) ? kIOReturnBadArgument : kIOReturnSuccess;
	if (done) {
		done(ref, r);
		return kIOReturnSuccess;
	}
	return r;
}

static IOReturn
pandora_aes_perform_md(void *self, IOMemoryDescriptor *in, IOMemoryDescriptor *out,
    unsigned long long bytes, void *iv, unsigned op, void *key, unsigned long long in_off,
    unsigned long long out_off, void (*done)(void *, int), void *ref)
{
	(void)self;
	pandora_aes_calls++;
	if (!in || !out || !iv || !key || !bytes || bytes > 65536 ||
	    (op != PANDORA_AES_ENCRYPT && op != PANDORA_AES_DECRYPT) ||
	    in_off > in->getLength() || bytes > in->getLength() - in_off ||
	    out_off > out->getLength() || bytes > out->getLength() - out_off) {
		return kIOReturnBadArgument;
	}
	uint8_t *buf = (uint8_t *)IOMallocData((vm_size_t)bytes);
	if (!buf) {
		return kIOReturnNoMemory;
	}
	IOReturn r = kIOReturnSuccess;
	if (in->readBytes(in_off, buf, (IOByteCount)bytes) != bytes ||
	    pandora_aes_cbc(buf, (size_t)bytes, (uint8_t *)iv, (const struct pandora_aes_key *)key, op) ||
	    out->writeBytes(out_off, buf, (IOByteCount)bytes) != bytes) {
		r = kIOReturnIOError;
	}
	bzero(buf, (size_t)bytes);
	IOFreeData(buf, (vm_size_t)bytes);
	if (done) {
		done(ref, r);
		return kIOReturnSuccess;
	}
	return r;
}

static void
pandora_aes_publish(IOService *parent)
{
	const OSSymbol *name = OSSymbol::withCString("IOAESAccelerator");
	const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : NULL;
	OSSafeReleaseNULL(name);
	OSObject *obj = NULL;
	IOService *aes = NULL;
	const char *why = NULL;
	if (!meta) {
		why = "no IOAESAccelerator class (IOCryptoAcceleratorFamily not prelinked)";
	} else if (meta->getClassSize() != AES_SIZE) {
		why = "unexpected IOAESAccelerator size";
	} else if (!parent) {
		why = "no parent";
	} else if (!(obj = meta->alloc()) || !(aes = OSDynamicCast(IOService, obj))) {
		why = "alloc failed";
	} else {
		uintptr_t *vptr = *(uintptr_t **)obj;
		const char *(*algorithm)(OSObject *) = (const char *(*)(OSObject *))vptr[AES_SLOT_NAME];
		const char *alg = algorithm(obj);
		if (vptr[-1] != 0 || vptr[-2] != 0 || !alg || strcmp(alg, "AES-CBC") != 0) {
			why = "IOAESAccelerator vtable layout differs";
		} else {
			memcpy(pandora_aes_vtable, vptr - 2, sizeof(pandora_aes_vtable));
			pandora_aes_vtable[2 + AES_SLOT_PERFORM] = (uintptr_t)pandora_aes_perform;
			pandora_aes_vtable[2 + AES_SLOT_PERFORM_MD] = (uintptr_t)pandora_aes_perform_md;
			*(uintptr_t **)obj = &pandora_aes_vtable[2];
			if (obj->getMetaClass() != meta) {
				why = "metaclass changed";
			} else if (!aes->init()) {
				why = "init failed";
			} else if (!aes->attach(parent) || !aes->inPlane(gIOServicePlane)) {
				why = "attach failed";
			}
		}
	}
	if (why) {
		printf("PANDORA AES: %s\n", why);
		if (pandora_pmgr_mode >= 2 && meta) {
			panic("PANDORA AES: %s", why);
		}
		return;
	}
	aes->setName("pandora-aes");
	aes->registerService(kIOServiceSynchronous);
	printf("PANDORA AES: IOAESAccelerator published (software AES-CBC)\n");
}

static void
pandora_pmgr_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	const OSMetaClass *meta = NULL;
	/* The prelinked ApplePMGR kext is loaded with AMFI, its dependent. */
	for (int i = 0; i < 300 && !(meta = pandora_pmgr_meta()); i++) {
		IOSleep(100);
	}
	IOService *nub = NULL;
	OSObject *obj = NULL;
	/* The thread starts before the platform expert is published (0153), and IOResources is
	 * not in the service plane yet either ("not registry member at registerService()").
	 * AMFI matches the nub by class only, so the service root (IOPlatformExpertDevice,
	 * registered by PE_init_iokit before matching starts) is the parent then. */
	IOService *platform = IOService::getPlatform();
	if (!platform) {
		platform = IOService::getServiceRoot();
	}
	const char *why = NULL;
	if (!meta) {
		why = "no ApplePMGRNub class after 30 s (ApplePMGR not prelinked or not loaded)";
	} else if (meta->getClassSize() != PMGR_NUB_SIZE) {
		why = "unexpected class size";
	} else if (!platform) {
		why = "no platform expert";
	} else if (!(obj = meta->alloc()) || !(nub = OSDynamicCast(IOService, obj))) {
		why = "alloc failed";
	} else if (*(volatile uintptr_t *)((uintptr_t)nub + PMGR_NUB_PROVIDER) != 0) {
		why = "provider field not empty";
	} else if (!nub->init()) {
		why = "init failed";
	} else if (!nub->attach(platform)) {
		why = "attach failed";
	} else if (!nub->inPlane(gIOServicePlane)) {
		why = "attached but not in the service plane";
	}
	if (why) {
		printf("PANDORA PMGR: %s\n", why);
		if (pandora_pmgr_mode >= 2) {
			panic("PANDORA PMGR: %s", why);
		}
		OSSafeReleaseNULL(obj);
	} else {
		nub->setName("pandora-pmgr");
		nub->registerService(kIOServiceSynchronous);
		/* The registry holds it from here on; AMFI keeps using it for the whole boot. */
		printf("PANDORA PMGR: ApplePMGRNub published (no provider: reads 0xffffffff, writes ignored)\n");
		if (pandora_pmgr_mode >= 3) {
			panic("PANDORA PMGR: ApplePMGRNub %p published under %s", nub, platform->getName());
		}
	}
	pandora_aes_publish(platform);
}

void
pandora_pmgr_init(void)
{
	(void)PE_parse_boot_argn("pandora_pmgr", &pandora_pmgr_mode, sizeof(pandora_pmgr_mode));
	if (!pandora_pmgr_mode) {
		return;
	}
	printf("PANDORA PMGR: publisher started (mode %d)\n", pandora_pmgr_mode);
	thread_t thread;
	if (kernel_thread_start(pandora_pmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
