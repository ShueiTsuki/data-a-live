/*
 * The Adreno 840's user interface: UniXNU NativeAdreno's user-client ABI version 1
 * (NativeAdrenoABI.h, MIT, copied unchanged as NativeAdrenoABIv1.h), so Turnip's Darwin backend (tu_knl_darwin.cc) can drive the GPU that
 * bsd/dev/pandora_gpu.c brings up. Only the wire format is NativeAdreno's; the buffer pool,
 * mappings, ring and fences are the pandora_gpu_* functions.
 *
 * Service class PandoraAdrenoGpu (name "NativeAdrenoGpu"; not NativeAdreno's class name
 * NativeAdrenoGpuPublished, which the fuxi lane's FuxiAdrenoUserClient.cpp already defines in this
 * kernel; Turnip takes TU_DARWIN_SERVICE=PandoraAdrenoGpu TU_DARWIN_OPEN_TYPE=0x4147), matched on IOResources; open type
 * 0x4147 makes an AGXDeviceUserClient (the class name Apple's sandbox profiles allow). Every
 * method is struct-in/struct-out with exact sizes; any selector also accepts the tunnel
 * {u32 'NTNL', u32 method, payload}, and method 0xffffffff is the probe Turnip sends.
 * Not yet: queues (11, 12). Status (8) answers zero counts (patch 0186); host import (16) pins the
 * caller's pages (patch 0187).
 */
#include <pexpert/arm64/board_config.h>
#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <stddef.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "NativeAdrenoABIv1.h"

extern "C" {
int pandora_gpu_open(void);
void pandora_gpu_close(int client);
void pandora_gpu_window(int client, uint64_t *start, uint64_t *size);
int pandora_gpu_bo_alloc(int client, uint64_t bytes, uint32_t align_log2, uint64_t *handle);
int pandora_gpu_bo_map(int client, uint64_t handle, uint64_t iova, uint32_t perm);
int pandora_gpu_bo_free(int client, uint64_t handle);
int pandora_gpu_bo_lease(int client, uint64_t handle, int delta, uint32_t *token, uint64_t *bytes,
    uint32_t *leases);
int pandora_gpu_token_phys(int client, uint32_t token, uint64_t *pa, uint64_t *bytes);
int pandora_gpu_bo_meta(int client, uint64_t handle, bool set, char *text, uint32_t *len);
int pandora_gpu_submit(int client, const uint64_t *iova, const uint64_t *bytes, unsigned n, uint64_t *fence);
int pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns);
uint64_t pandora_gpu_timestamp(void);
uint32_t pandora_gpu_chip_id(void);
int pandora_gpu_bo_import(int client, const uint64_t *pages, uint32_t n, void *ref, bool readonly,
    uint64_t *handle);
void pandora_gpu_release_ref(void *ref);
}

#define PANDORA_GPU_OPEN_TYPE   0x4147u
#define PANDORA_GPU_TUNNEL      0x4e544e4cu     /* 'NTNL' */
#define PANDORA_GPU_PROBE       0xffffffffu
/* The upstream submission (16 buffers); ours has NativeAdrenoAbiMaxBuffers = 128 (tools/turnip/abi_128.py). */
#define PANDORA_GPU_SUBMIT16_BYTES (offsetof(NativeAdrenoAbiSubmit, buffers) + 16 * sizeof(NativeAdrenoAbiSubmitBuffer))
#define PANDORA_GPU_TOKENS      4096u

static IOReturn
pandora_gpu_ret(int err)
{
	switch (err) {
	case 0: return kIOReturnSuccess;
	case EINVAL: return kIOReturnBadArgument;
	case ENOMEM: return kIOReturnNoMemory;
	case EBUSY: return kIOReturnBusy;
	case ETIMEDOUT: return kIOReturnTimeout;
	case ENXIO: return kIOReturnNotReady;
	default: return kIOReturnError;
	}
}

class AGXDeviceUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AGXDeviceUserClient);

public:
	int client_ = -1;
	task_t task_ = TASK_NULL;                   /* the opening task: BoImport pins its pages */
	IOMemoryDescriptor **views_ = nullptr;      /* per CPU-view token, kept until its last lease */

	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn method(uint32_t m, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len);
};

OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient);

void
AGXDeviceUserClient::free(void)
{
	if (views_ != nullptr) {
		for (unsigned i = 0; i < PANDORA_GPU_TOKENS; i++) {
			if (views_[i] != nullptr) {
				views_[i]->release();
			}
		}
		IOFree(views_, PANDORA_GPU_TOKENS * sizeof(views_[0]));
	}
	IOUserClient::free();
}

IOReturn
AGXDeviceUserClient::clientClose(void)
{
	if (client_ >= 0) {
		pandora_gpu_close(client_);
		client_ = -1;
	}
	terminate();
	return kIOReturnSuccess;
}

/* TYPE is a token from BoCpuMap: the buffer's physical range (pool pages, wired for good). */
IOReturn
AGXDeviceUserClient::clientMemoryForType(UInt32 type, __unused IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	uint64_t pa = 0, bytes = 0;
	if (client_ < 0 || type < 1 || type > PANDORA_GPU_TOKENS ||
	    pandora_gpu_token_phys(client_, type, &pa, &bytes)) {
		return kIOReturnBadArgument;
	}
	IOMemoryDescriptor *md = views_[type - 1];
	if (md == nullptr) {
		md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)pa, (IOByteCount)bytes,
		    kIODirectionInOut);
		if (md == nullptr) {
			return kIOReturnNoMemory;
		}
		views_[type - 1] = md;
	}
	md->retain();
	*memory = md;
	return kIOReturnSuccess;
}

/* Exact input and output sizes per method (NativeAdrenoABI.h); 0xffff = not supported. */
static const uint16_t pandora_gpu_sizes[17][2] = {
	{ 0xffff, 0xffff },
	{ 0, sizeof(NativeAdrenoAbiParams) },
	{ sizeof(NativeAdrenoAbiBoAlloc), sizeof(NativeAdrenoAbiBoAllocReply) },
	{ sizeof(NativeAdrenoAbiBoMap), sizeof(NativeAdrenoAbiBoMapReply) },
	{ sizeof(NativeAdrenoAbiBoFree), 0 },
	{ sizeof(NativeAdrenoAbiSubmit), sizeof(NativeAdrenoAbiSubmitReply) },
	{ sizeof(NativeAdrenoAbiWaitFence), 0 },
	{ 0, sizeof(NativeAdrenoAbiTimestamp) },
	{ 0, sizeof(NativeAdrenoAbiStatus) },
	{ sizeof(NativeAdrenoAbiMetadata), 0 },
	{ sizeof(NativeAdrenoAbiMetadata), sizeof(NativeAdrenoAbiMetadata) },
	{ 0xffff, 0xffff },
	{ 0xffff, 0xffff },
	{ sizeof(NativeAdrenoAbiBoCpuMap), sizeof(NativeAdrenoAbiBoCpuMapReply) },
	{ sizeof(NativeAdrenoAbiBoCpuUnmap), 0 },
	{ 0, sizeof(NativeAdrenoAbiDeviceInfo) },
	{ sizeof(NativeAdrenoAbiBoImport), sizeof(NativeAdrenoAbiBoAllocReply) },
};

IOReturn
AGXDeviceUserClient::method(uint32_t m, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len)
{
	if (m > 16 || pandora_gpu_sizes[m][0] == 0xffff) {
		return kIOReturnUnsupported;
	}
	/* Submit also takes the upstream 16-buffer layout (416 bytes: tools/pgputest, older Turnip). */
	const bool legacySubmit = m == NativeAdrenoMethodSubmit && in_len == PANDORA_GPU_SUBMIT16_BYTES;
	if ((in_len != pandora_gpu_sizes[m][0] && !legacySubmit) || out_len != pandora_gpu_sizes[m][1]) {
		return kIOReturnBadArgument;
	}
	switch (m) {
	case NativeAdrenoMethodQuery: {
		NativeAdrenoAbiParams p = { NativeAdrenoAbiVersion, 32, 36, NativeAdrenoParamsHostImport,
			1ull << NativeAdrenoAbiMaxBoBytesShift };
		memcpy(out, &p, sizeof(p));
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodBoAlloc: {
		NativeAdrenoAbiBoAlloc a;
		NativeAdrenoAbiBoAllocReply r;
		memcpy(&a, in, sizeof(a));
		if (a.contiguous > 1) {
			return kIOReturnBadArgument;
		}
		int err = pandora_gpu_bo_alloc(client_, a.bytes, a.align_log2, &r.handle);
		memcpy(out, &r, sizeof(r));
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodBoMap: {
		NativeAdrenoAbiBoMap a;
		memcpy(&a, in, sizeof(a));
		/* Mapped normal non-cacheable, outer shareable, as Turnip asks (writeback 0, share 2). */
		if (a.reserved || a.writeback || a.shareability != 2) {
			return kIOReturnUnsupported;
		}
		NativeAdrenoAbiBoMapReply r = { a.iova };
		memcpy(out, &r, sizeof(r));
		return pandora_gpu_ret(pandora_gpu_bo_map(client_, a.handle, a.iova, a.permissions));
	}
	case NativeAdrenoMethodBoFree: {
		NativeAdrenoAbiBoFree a;
		memcpy(&a, in, sizeof(a));
		return pandora_gpu_ret(pandora_gpu_bo_free(client_, a.handle));
	}
	case NativeAdrenoMethodSubmit: {
		/* The header, then each buffer straight from the input: the 128-buffer submission (3 KB)
		 * stays off the kernel stack. */
		NativeAdrenoAbiSubmit a;
		const size_t head = offsetof(NativeAdrenoAbiSubmit, buffers);
		memcpy(&a, in, head);
		const uint32_t max = (in_len - head) / sizeof(NativeAdrenoAbiSubmitBuffer);
		if (!a.buffer_count || a.buffer_count > max || a.reserved || a.queue) {
			return kIOReturnBadArgument;
		}
		if (a.wait_fence && pandora_gpu_wait(a.wait_fence, a.wait_timeout_ns)) {
			return kIOReturnTimeout;
		}
		const vm_size_t list = a.buffer_count * sizeof(uint64_t);
		uint64_t *iova = (uint64_t *)IOMallocData(list), *bytes = (uint64_t *)IOMallocData(list);
		int err = ENOMEM;
		NativeAdrenoAbiSubmitReply r = { 0 };
		if (iova != nullptr && bytes != nullptr) {
			for (unsigned i = 0; i < a.buffer_count; i++) {
				NativeAdrenoAbiSubmitBuffer b;
				memcpy(&b, in + head + i * sizeof(b), sizeof(b));
				iova[i] = b.iova;
				bytes[i] = b.bytes;
			}
			err = pandora_gpu_submit(client_, iova, bytes, a.buffer_count, &r.fence);
		}
		if (iova != nullptr) {
			IOFreeData(iova, list);
		}
		if (bytes != nullptr) {
			IOFreeData(bytes, list);
		}
		memcpy(out, &r, sizeof(r));
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodWaitFence: {
		NativeAdrenoAbiWaitFence a;
		memcpy(&a, in, sizeof(a));
		return pandora_gpu_ret(pandora_gpu_wait(a.fence, a.timeout_ns));
	}
	case NativeAdrenoMethodGetStatus: {
		/* No GPU fault or suspend is counted yet: both stay 0, which Turnip reads as "context intact". */
		NativeAdrenoAbiStatus r = { 0, 0 };
		memcpy(out, &r, sizeof(r));
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodGetTimestamp: {
		NativeAdrenoAbiTimestamp r = { pandora_gpu_timestamp() };
		memcpy(out, &r, sizeof(r));
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodSetMetadata:
	case NativeAdrenoMethodGetMetadata: {
		NativeAdrenoAbiMetadata a;
		memcpy(&a, in, sizeof(a));
		bool set = m == NativeAdrenoMethodSetMetadata;
		if (a.reserved || (set && a.bytes > NativeAdrenoAbiMetadataBytes)) {
			return kIOReturnBadArgument;
		}
		uint32_t len = set ? a.bytes : 0;
		int err = pandora_gpu_bo_meta(client_, a.handle, set, a.text, &len);
		if (!set && !err) {
			if (len > a.bytes) {
				return kIOReturnNoSpace;
			}
			a.bytes = len;
			memcpy(out, &a, sizeof(a));
		}
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodBoCpuMap: {
		NativeAdrenoAbiBoCpuMap a;
		NativeAdrenoAbiBoCpuMapReply r = { 0, 0, 0 };
		uint32_t leases;
		memcpy(&a, in, sizeof(a));
		int err = pandora_gpu_bo_lease(client_, a.handle, 1, &r.token, &r.bytes, &leases);
		memcpy(out, &r, sizeof(r));
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodBoCpuUnmap: {
		NativeAdrenoAbiBoCpuUnmap a;
		uint32_t token, leases;
		uint64_t bytes;
		memcpy(&a, in, sizeof(a));
		if (a.reserved || pandora_gpu_bo_lease(client_, a.handle, 0, &token, &bytes, &leases) ||
		    token != a.token || !leases) {
			return kIOReturnBadArgument;
		}
		int err = pandora_gpu_bo_lease(client_, a.handle, -1, &token, &bytes, &leases);
		if (!err && !leases && views_[token - 1] != nullptr) {
			views_[token - 1]->release();
			views_[token - 1] = nullptr;
		}
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodBoImport: {
		/* The caller's own pages [address, address + bytes), wired by a descriptor that stays
		 * prepared until the buffer is freed (pandora_gpu_release_ref). */
		NativeAdrenoAbiBoImport a;
		NativeAdrenoAbiBoAllocReply r = { 0 };
		memcpy(&a, in, sizeof(a));
		if (!a.bytes || ((a.address | a.bytes) & 0xfff) || a.bytes > (1ull << NativeAdrenoAbiMaxBoBytesShift)) {
			return kIOReturnBadArgument;
		}
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)a.address,
		    (mach_vm_size_t)a.bytes, kIODirectionInOut, task_);
		if (md == nullptr) {
			return kIOReturnNoMemory;
		}
		IOReturn ret = md->prepare();
		bool readonly = false;
		if (ret == KERN_PROTECTION_FAILURE) {
			/* A read-only mapping (bb9: a surface backboardd only reads): pinned for the
			 * device to read, and mapped read-only for the GPU. */
			md->release();
			md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)a.address,
			    (mach_vm_size_t)a.bytes, kIODirectionOut, task_);
			if (md == nullptr) {
				return kIOReturnNoMemory;
			}
			ret = md->prepare();
			readonly = true;
		}
		if (ret != kIOReturnSuccess) {
			md->release();
			return ret;
		}
		const uint32_t n = (uint32_t)(a.bytes >> 12);
		uint64_t *pages = (uint64_t *)IOMallocData(n * sizeof(uint64_t));
		bool whole = pages != nullptr;
		for (uint32_t i = 0; whole && i < n; i++) {
			IOByteCount len = 0;
			pages[i] = md->getPhysicalSegment((IOByteCount)i << 12, &len, kIOMemoryMapperNone);
			whole = pages[i] && !(pages[i] & 0xfff) && len >= 0x1000;
		}
		int err = whole ? pandora_gpu_bo_import(client_, pages, n, md, readonly, &r.handle) : EINVAL;
		if (pages != nullptr) {
			IOFreeData(pages, n * sizeof(uint64_t));
		}
		if (err) {
			md->complete();
			md->release();
		}
		memcpy(out, &r, sizeof(r));
		return pandora_gpu_ret(err);
	}
	case NativeAdrenoMethodGetDeviceInfo: {
		/* Linux a6xx_catalog (A840: chip 0x44050a01, 18 MiB GMEM; the slice count in bits 4..7 as
		 * a8xx_gpu_get_slice_info adds it, 0x44050a31 in Mesa's table), a8xx_hw_init (GMEM range at
		 * 64 MiB), a6xx uche_trap_base, ubwc_config kaanapali (UBWC 6.0, highest bank bit 16;
		 * swizzle levels 2 and 3; 8-channel macrotiling from UBWC 3.1). */
		NativeAdrenoAbiDeviceInfo d = {};
		d.chip_id = pandora_gpu_chip_id();
		d.gmem_bytes = 18u << 20;
		d.gmem_base = 64ull << 20;
		d.uche_trap_base = 0x1fffffffff000ull;
		pandora_gpu_window(client_, &d.va_start, &d.va_size);
		d.highest_bank_bit = 16;
		d.ubwc_swizzle = 6;
		d.macrotile_mode = 1;
		d.ubwc_version = 6;
		d.flags = NativeAdrenoDeviceClientIova;
		memcpy(out, &d, sizeof(d));
		return kIOReturnSuccess;
	}
	}
	return kIOReturnUnsupported;
}

IOReturn
AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (client_ < 0 || args->scalarInputCount || args->scalarOutputCount ||
	    args->structureInputDescriptor != nullptr || args->structureOutputDescriptor != nullptr) {
		return kIOReturnBadArgument;
	}
	const uint8_t *in = (const uint8_t *)args->structureInput;
	uint32_t in_len = args->structureInputSize, m = selector, magic = 0;
	if (in_len >= 8) {
		memcpy(&magic, in, 4);
	}
	if (magic == PANDORA_GPU_TUNNEL) {
		memcpy(&m, in + 4, 4);
		in += 8;
		in_len -= 8;
		if (m == PANDORA_GPU_PROBE) {
			args->structureOutputSize = 0;
			return kIOReturnSuccess;
		}
	}
	uint8_t out[sizeof(NativeAdrenoAbiMetadata)];
	uint32_t out_len = args->structureOutputSize;
	if (out_len > sizeof(out)) {
		return kIOReturnBadArgument;
	}
	IOReturn ret = method(m, in, in_len, out, out_len);
	if (ret == kIOReturnSuccess && out_len) {
		memcpy(args->structureOutput, out, out_len);
	}
	if (ret == kIOReturnTimeout && m == NativeAdrenoMethodWaitFence) {
		/* Turnip polls fences with short timeouts: counted, a line per 1000 (0193; bb19 filled the
		 * log ring with them while the GPU was stuck). */
		static uint32_t timeouts;
		if ((++timeouts % 1000) == 1) {
			IOLog("PANDORA GPU UC: client %d fence waits timed out %u times\n", client_, timeouts);
		}
	} else if (ret != kIOReturnSuccess) {
		IOLog("PANDORA GPU UC: client %d method %u in %u out %u -> 0x%x\n", client_, m, in_len, out_len, ret);
	}
	return ret;
}

class PandoraAdrenoGpu : public IOService {
	OSDeclareDefaultStructors(PandoraAdrenoGpu);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(PandoraAdrenoGpu, IOService);

bool
PandoraAdrenoGpu::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setName("NativeAdrenoGpu");
	registerService();
	IOLog("PANDORA GPU UC: PandoraAdrenoGpu registered\n");
	return true;
}

/* The open both services share: type 0x4147 makes an AGXDeviceUserClient attached to OWNER. */
static IOReturn
pandora_gpu_new_client(IOService *owner, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
	if (type != PANDORA_GPU_OPEN_TYPE) {
		return kIOReturnUnsupported;
	}
	int c = pandora_gpu_open();
	if (c < 0) {
		IOLog("PANDORA GPU UC: open refused (GPU not up or no free window)\n");
		return kIOReturnNotReady;
	}
	AGXDeviceUserClient *uc = OSTypeAlloc(AGXDeviceUserClient);
	void *views = IOMallocZero(PANDORA_GPU_TOKENS * sizeof(IOMemoryDescriptor *));
	if (uc == nullptr || views == nullptr || !uc->initWithTask(owningTask, securityID, type)) {
		if (views != nullptr) {
			IOFree(views, PANDORA_GPU_TOKENS * sizeof(IOMemoryDescriptor *));
		}
		if (uc != nullptr) {
			uc->release();
		}
		pandora_gpu_close(c);
		return kIOReturnNoMemory;
	}
	uc->client_ = c;
	uc->task_ = owningTask;
	uc->views_ = (IOMemoryDescriptor **)views;
	if (!uc->attach(owner) || !uc->start(owner)) {
		uc->detach(owner);
		uc->clientClose();
		uc->release();
		return kIOReturnError;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("PANDORA GPU UC: client %d opened by %s through %s\n", c, name, owner->getMetaClass()->getClassName());
	*handler = uc;
	return kIOReturnSuccess;
}

IOReturn
PandoraAdrenoGpu::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    __unused OSDictionary *properties, IOUserClient **handler)
{
	return pandora_gpu_new_client(this, owningTask, securityID, type, handler);
}

/*
 * Metal's discovery (patch 0184): Metal.framework looks up IOServiceMatching("IOAcceleratorES")
 * and loads /System/Library/Extensions/<MetalPluginName>.bundle, class MetalPluginClassName; Turnip's
 * Darwin backend, without TU_DARWIN_SERVICE, also opens IOAcceleratorES (type 0x4147, NTNL tunnel).
 * IOAcceleratorES here is the empty in-kernel class of the fuxi lane (FuxiAdrenoUserClient.cpp,
 * compiled into this kernel; Apple's IOGPUFamily is not loaded), declared again to derive from it.
 * The class is named AGXAccelerator as UniXNU's is, the name Apple's sandbox profiles accept. It is
 * published only with boot-arg pandora_gpu_metal=1: once it exists, every process that starts
 * Metal (backboardd first) finds it and goes looking for the plugin.
 */
class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

class AGXAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(AGXAccelerator);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES);

bool
AGXAccelerator::start(IOService *provider)
{
	int metal = 0;
	(void)PE_parse_boot_argn("pandora_gpu_metal", &metal, sizeof(metal));
	if (!metal || !IOService::start(provider)) {
		return false;
	}
	setName("NativeAdrenoGpu");
	setProperty("MetalPluginName", "NativeAdrenoMetal");
	setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
	registerService();
	IOLog("PANDORA GPU UC: AGXAccelerator (IOAcceleratorES) registered, plugin NativeAdrenoMetal\n");
	return true;
}

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    __unused OSDictionary *properties, IOUserClient **handler)
{
	return pandora_gpu_new_client(this, owningTask, securityID, type, handler);
}
/* For bsd/dev/pandora_gpu.c: unpin an imported buffer (its BoImport descriptor). */
extern "C" void
pandora_gpu_release_ref(void *ref)
{
	IOMemoryDescriptor *md = (IOMemoryDescriptor *)ref;
	if (md != nullptr) {
		md->complete();
		md->release();
	}
}
#endif /* SWEET */
