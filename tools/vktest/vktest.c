/*
 * vktest: the first Vulkan work on the Adreno 840, through Turnip (Mesa) with UniXNU's Darwin
 * kernel backend over our user client (iokit/Kernel/arm/PandoraAdrenoGpu.cpp). Three parts, each
 * logged on its own so a failure shows where it is:
 *   1. instance and device: the physical device Turnip reports (name, chip from the device info)
 *   2. buffer: vkCmdFillBuffer and vkCmdUpdateBuffer into host-visible memory, read back
 *   3. image: vkCmdClearColorImage on a 64x64 RGBA8 image, copied to a buffer, read back
 * The driver is dlopen'd from /usr/lib/libvulkan_freedreno.dylib (no Vulkan loader on iOS) and
 * reached through vk_icdGetInstanceProcAddr. stdout and stderr go to /dev/console, so Mesa's own
 * messages land in the kernel log next to the "VKTEST:" lines.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#define DRIVER "/usr/lib/libvulkan_freedreno.dylib"

static int con = -1;
static unsigned failures;

static void
say(const char *fmt, ...)
{
	char buf[512];
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

static int
check(const char *what, VkResult r)
{
	say("VKTEST: %s -> %d%s", what, r, r ? " FAIL" : "");
	failures += r != VK_SUCCESS;
	return r != VK_SUCCESS;
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

int
main(void)
{
	con = open("/dev/console", O_WRONLY);
	if (con >= 0) {
		dup2(con, 1);
		dup2(con, 2);
	}
	sleep(5);
	setenv("TU_DARWIN_SERVICE", "PandoraAdrenoGpu", 1);
	setenv("TU_DARWIN_OPEN_TYPE", "0x4147", 1);
	setenv("TU_DEBUG", "sysmem,nocb", 1);     /* no GMEM binning, no concurrent binning yet */
	say("VKTEST: start");

	/* ---- part 1: instance and device ---- */
	void *drv = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);
	if (!drv) {
		say("VKTEST: dlopen %s FAIL: %s", DRIVER, dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(drv, "vk_icdGetInstanceProcAddr");
	VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(drv,
	    "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (negotiate) {
		uint32_t v = 5;
		VkResult nr = negotiate(&v);
		say("VKTEST: loader interface version %u (%d)", v, nr);
	}
	if (!gipa) {
		say("VKTEST: no vk_icdGetInstanceProcAddr FAIL");
		return 1;
	}
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, "pandora", 1, VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
	if (check("part 1: vkCreateInstance", vkCreateInstance(&ici, NULL, &inst))) {
		return 1;
	}
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceProperties);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkCreateDevice);
	IFN(vkGetDeviceProcAddr);
	uint32_t n = 1;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (check("part 1: vkEnumeratePhysicalDevices", r == VK_INCOMPLETE ? VK_SUCCESS : r) || !n || !pd) {
		say("VKTEST: no physical device FAIL");
		return 1;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	say("VKTEST: device '%s' vendor %#x device %#x api %u.%u.%u driver %#x", props.deviceName, props.vendorID,
	    props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	    VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
	vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
		say("VKTEST: memory type %u flags %#x heap %u", i, memprops.memoryTypes[i].propertyFlags,
		    memprops.memoryTypes[i].heapIndex);
	}
	uint32_t nq = 8, qf = UINT32_MAX;
	VkQueueFamilyProperties qfp[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qfp);
	for (uint32_t i = 0; i < nq && qf == UINT32_MAX; i++) {
		if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qf = i;
		}
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
	if (check("part 1: vkCreateDevice", vkCreateDevice(pd, &dci, NULL, &dev))) {
		return 1;
	}
	DFN(vkGetDeviceQueue);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkInvalidateMappedMemoryRanges);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdFillBuffer);
	DFN(vkCmdUpdateBuffer);
	DFN(vkCmdPipelineBarrier);
	DFN(vkCmdClearColorImage);
	DFN(vkCmdCopyImageToBuffer);
	DFN(vkCreateImage);
	DFN(vkGetImageMemoryRequirements);
	DFN(vkBindImageMemory);
	DFN(vkCreateFence);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkResetFences);
	DFN(vkResetCommandBuffer);
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);
	say("VKTEST: part 1 done (queue family %u)", qf);

	/* ---- part 2: fill and update a host-visible buffer ---- */
	const VkDeviceSize size = 64 * 1024;
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
	VkBuffer buf;
	if (check("part 2: vkCreateBuffer", vkCreateBuffer(dev, &bci, NULL, &buf))) {
		return 1;
	}
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	uint32_t mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
	VkDeviceMemory mem;
	if (check("part 2: vkAllocateMemory", vkAllocateMemory(dev, &mai, NULL, &mem)) ||
	    check("part 2: vkBindBufferMemory", vkBindBufferMemory(dev, buf, mem, 0))) {
		return 1;
	}
	volatile uint32_t *p = NULL;
	if (check("part 2: vkMapMemory", vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p))) {
		return 1;
	}
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
		VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf };
	VkCommandPool pool;
	VkCommandBuffer cb;
	VkFence fence;
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0,
		VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
	if (check("part 2: vkCreateCommandPool", vkCreateCommandPool(dev, &cpi, NULL, &pool))) {
		return 1;
	}
	cai.commandPool = pool;
	if (check("part 2: vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cai, &cb)) ||
	    check("part 2: vkCreateFence", vkCreateFence(dev, &fci, NULL, &fence))) {
		return 1;
	}
	VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
	VkMemoryBarrier toHost = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
	const uint32_t words[4] = { 0x16161616, 0x26262626, 0x36363636, 0x46464646 };
	vkBeginCommandBuffer(cb, &cbi);
	vkCmdFillBuffer(cb, buf, 0, 16 * 1024, 0x16161616);
	vkCmdUpdateBuffer(cb, buf, 32 * 1024, sizeof(words), words);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
	vkEndCommandBuffer(cb);
	if (!check("part 2: vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence)) &&
	    !check("part 2: vkWaitForFences", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) {
		VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
		vkInvalidateMappedMemoryRanges(dev, 1, &range);
		unsigned bad = 0;
		for (unsigned i = 0; i < 16 * 1024 / 4; i++) {
			bad += p[i] != 0x16161616;
		}
		for (unsigned i = 0; i < 4; i++) {
			bad += p[32 * 1024 / 4 + i] != words[i];
		}
		say("VKTEST: part 2: fill %08x..%08x update %08x %08x %08x %08x, %u words wrong%s", p[0],
		    p[16 * 1024 / 4 - 1], p[8192], p[8193], p[8194], p[8195], bad, bad ? " FAIL" : "");
		failures += bad != 0;
	}

	/* ---- part 3: clear an image, copy it to the buffer ---- */
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
		{ 64, 64, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
		VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	VkDeviceMemory imem;
	if (check("part 3: vkCreateImage", vkCreateImage(dev, &ii, NULL, &img))) {
		goto done;
	}
	vkGetImageMemoryRequirements(dev, img, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX) {
		mai.memoryTypeIndex = memtype(mr.memoryTypeBits, 0);
	}
	if (check("part 3: vkAllocateMemory", vkAllocateMemory(dev, &mai, NULL, &imem)) ||
	    check("part 3: vkBindImageMemory", vkBindImageMemory(dev, img, imem, 0))) {
		goto done;
	}
	VkImageSubresourceRange sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED, img, sub };
	VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, sub };
	VkClearColorValue color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
	VkBufferImageCopy copy = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 64, 64, 1 } };
	vkResetFences(dev, 1, &fence);
	vkResetCommandBuffer(cb, 0);
	vkBeginCommandBuffer(cb, &cbi);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &sub);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
	vkEndCommandBuffer(cb);
	if (!check("part 3: vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence)) &&
	    !check("part 3: vkWaitForFences", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) {
		VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
		vkInvalidateMappedMemoryRanges(dev, 1, &range);
		/* R 0.25 -> 0x40, G 0.5 -> 0x80, B 0.75 -> 0xbf or 0xc0, A 1.0 -> 0xff (little-endian word). */
		unsigned bad = 0;
		for (unsigned i = 0; i < 64 * 64; i++) {
			uint32_t v = p[i];
			uint32_t blue = (v >> 16) & 0xff;
			bad += (v & 0xff) != 0x40 || ((v >> 8) & 0xff) != 0x80 || (blue != 0xbf && blue != 0xc0) ||
			    (v >> 24) != 0xff;
		}
		say("VKTEST: part 3: pixels %08x %08x ... %08x, %u of 4096 wrong%s", p[0], p[1], p[4095], bad,
		    bad ? " FAIL" : "");
		failures += bad != 0;
	}
done:
	say("VKTEST: %s (%u failures)", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
