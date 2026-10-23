/*
 * vktest: the first Vulkan work on the Adreno 840, through Turnip (Mesa) with UniXNU's Darwin
 * kernel backend over our user client (iokit/Kernel/arm/PandoraAdrenoGpu.cpp). Three parts, each
 * logged on its own so a failure shows where it is:
 *   1. instance and device: the physical device Turnip reports (name, chip from the device info)
 *   2. buffer: vkCmdFillBuffer and vkCmdUpdateBuffer into host-visible memory, read back
 *   3. image: vkCmdClearColorImage on a 64x64 RGBA8 image, copied to a buffer, read back
 *   4. compute: fill.comp (64 x 64 invocations) compiled by Turnip on the phone, run, read back
 *   5. draw: a triangle (tri.vert, tri.frag) into a 64x64 render target, copied back and checked
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
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <spawn.h>
#include <vulkan/vulkan_core.h>
#include "fill_spv.h"           /* fill.comp as SPIR-V, made by glslangValidator in build_ios.sh */
#include "tri_vert_spv.h"       /* tri.vert */
#include "tri_frag_spv.h"       /* tri.frag */

#define DRIVER "/usr/lib/libvulkan_freedreno.dylib"
#define MTLTEST "/usr/libexec/mtltest"
extern char **environ;

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
	/* With pandora_gpu_metal=1 the kernel publishes AGXAccelerator (IOAcceleratorES), and Turnip is
	 * left to its defaults (IOAcceleratorES, 0x4147, the NTNL tunnel), as inside the Metal plugin;
	 * otherwise the direct service PandoraAdrenoGpu. */
	char args[1024] = "";
	size_t alen = sizeof(args) - 1;
	(void)sysctlbyname("kern.bootargs", args, &alen, NULL, 0);
	int metal = strstr(args, "pandora_gpu_metal=1") != NULL;
	if (!metal) {
		setenv("TU_DARWIN_SERVICE", "PandoraAdrenoGpu", 1);
		setenv("TU_DARWIN_OPEN_TYPE", "0x4147", 1);
	}
	setenv("TU_DEBUG", "sysmem,nocb", 1);     /* no GMEM binning, no concurrent binning yet */
	say("VKTEST: start (service %s)", metal ? "IOAcceleratorES, Turnip's default" : "PandoraAdrenoGpu");

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
	/* ---- part 4: a compute shader (fill.comp), compiled by Turnip on the phone ---- */
	{
		DFN(vkCreateShaderModule);
		DFN(vkCreateDescriptorSetLayout);
		DFN(vkCreatePipelineLayout);
		DFN(vkCreateComputePipelines);
		DFN(vkCreateDescriptorPool);
		DFN(vkAllocateDescriptorSets);
		DFN(vkUpdateDescriptorSets);
		DFN(vkCmdBindPipeline);
		DFN(vkCmdBindDescriptorSets);
		DFN(vkCmdDispatch);
		VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fill_spv), fill_spv };
		VkShaderModule sm;
		VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bind };
		VkDescriptorSetLayout dsl;
		VkPipelineLayout pl;
		VkPipeline pipe;
		VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
		VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &dps };
		VkDescriptorPool dp;
		VkDescriptorSet ds;
		if (check("part 4: vkCreateShaderModule", vkCreateShaderModule(dev, &smi, NULL, &sm)) ||
		    check("part 4: vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl))) {
			goto done;
		}
		VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
		if (check("part 4: vkCreatePipelineLayout", vkCreatePipelineLayout(dev, &pli, NULL, &pl))) {
			goto done;
		}
		VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
			pl, VK_NULL_HANDLE, -1 };
		if (check("part 4: vkCreateComputePipelines (Turnip compiles the shader)",
		    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) ||
		    check("part 4: vkCreateDescriptorPool", vkCreateDescriptorPool(dev, &dpi, NULL, &dp))) {
			goto done;
		}
		VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
		if (check("part 4: vkAllocateDescriptorSets", vkAllocateDescriptorSets(dev, &dai, &ds))) {
			goto done;
		}
		VkDescriptorBufferInfo dbi = { buf, 0, 4096 * 4 };
		VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
		vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
		VkMemoryBarrier shToHost = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
		vkResetFences(dev, 1, &fence);
		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &cbi);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		vkCmdDispatch(cb, 64, 1, 1);                    /* 64 workgroups x 64 = 4096 words */
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shToHost,
		    0, NULL, 0, NULL);
		vkEndCommandBuffer(cb);
		if (!check("part 4: vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence)) &&
		    !check("part 4: vkWaitForFences", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) {
			VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
			vkInvalidateMappedMemoryRanges(dev, 1, &range);
			unsigned bad = 0, first = 4096;
			for (unsigned i = 0; i < 4096; i++) {
				if (p[i] != i * 3u + 0x1000u) {
					bad++;
					first = first < i ? first : i;
				}
			}
			say("VKTEST: part 4: words %08x %08x ... %08x, %u of 4096 wrong (first at %u)%s", p[0], p[1], p[4095],
			    bad, bad ? first : 0, bad ? " FAIL" : "");
			failures += bad != 0;
		}
	}
	/* ---- part 5: a triangle (tri.vert, tri.frag) into a 64x64 render target, copied back ---- */
	{
		DFN(vkCreateShaderModule);
		DFN(vkCreatePipelineLayout);
		DFN(vkCreateImageView);
		DFN(vkCreateRenderPass);
		DFN(vkCreateFramebuffer);
		DFN(vkCreateGraphicsPipelines);
		DFN(vkCmdBeginRenderPass);
		DFN(vkCmdEndRenderPass);
		DFN(vkCmdBindPipeline);
		DFN(vkCmdDraw);
		VkImageCreateInfo ti = ii;
		ti.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VkImage timg;
		VkDeviceMemory tmem;
		if (check("part 5: vkCreateImage", vkCreateImage(dev, &ti, NULL, &timg))) {
			goto done;
		}
		vkGetImageMemoryRequirements(dev, timg, &mr);
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (check("part 5: vkAllocateMemory", vkAllocateMemory(dev, &mai, NULL, &tmem)) ||
		    check("part 5: vkBindImageMemory", vkBindImageMemory(dev, timg, tmem, 0))) {
			goto done;
		}
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, timg, VK_IMAGE_VIEW_TYPE_2D,
			VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		VkImageView view;
		VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
		VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
		VkSubpassDependency dep = { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 };
		VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sp, 1, &dep };
		VkRenderPass rp;
		if (check("part 5: vkCreateImageView", vkCreateImageView(dev, &vi, NULL, &view)) ||
		    check("part 5: vkCreateRenderPass", vkCreateRenderPass(dev, &rpi, NULL, &rp))) {
			goto done;
		}
		VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, 64, 64, 1 };
		VkFramebuffer fb;
		VkShaderModuleCreateInfo vsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(tri_vert_spv), tri_vert_spv };
		VkShaderModuleCreateInfo fsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(tri_frag_spv), tri_frag_spv };
		VkShaderModule vs, fs;
		VkPipelineLayoutCreateInfo epli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL };
		VkPipelineLayout epl;
		if (check("part 5: vkCreateFramebuffer", vkCreateFramebuffer(dev, &fbi, NULL, &fb)) ||
		    check("part 5: vkCreateShaderModule (vertex)", vkCreateShaderModule(dev, &vsi, NULL, &vs)) ||
		    check("part 5: vkCreateShaderModule (fragment)", vkCreateShaderModule(dev, &fsi, NULL, &fs)) ||
		    check("part 5: vkCreatePipelineLayout", vkCreatePipelineLayout(dev, &epli, NULL, &epl))) {
			goto done;
		}
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
		};
		VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
		VkViewport vp = { 0, 0, 64, 64, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { 64, 64 } };
		VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
			VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
			VK_SAMPLE_COUNT_1_BIT };
		VkPipelineColorBlendAttachmentState cba = { VK_FALSE };
		cba.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
			VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba };
		VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vin, &ia,
			NULL, &vps, &rs, &ms, NULL, &cbs, NULL, epl, rp, 0, VK_NULL_HANDLE, -1 };
		VkPipeline gp;
		if (check("part 5: vkCreateGraphicsPipelines (Turnip compiles both shaders)",
		    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &gp))) {
			goto done;
		}
		VkClearValue black = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, sc, 1, &black };
		vkResetFences(dev, 1, &fence);
		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &cbi);
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		vkCmdCopyImageToBuffer(cb, timg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
		vkEndCommandBuffer(cb);
		if (!check("part 5: vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence)) &&
		    !check("part 5: vkWaitForFences", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) {
			VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
			vkInvalidateMappedMemoryRanges(dev, 1, &range);
			/* Covered: x + y < 63 (the diagonal from top-right to bottom-left); exact edge pixels
			 * depend on the rasterizer's rules, so only pixels clearly inside or outside count. */
			unsigned green = 0, black_px = 0, other = 0, bad = 0;
			for (unsigned y = 0; y < 64; y++) {
				for (unsigned x = 0; x < 64; x++) {
					uint32_t v = p[y * 64 + x];
					green += v == 0xff00ff00u;
					black_px += v == 0xff000000u;
					other += v != 0xff00ff00u && v != 0xff000000u;
					if (x + y + 2 < 63) {
						bad += v != 0xff00ff00u;
					} else if (x + y > 63 + 2) {
						bad += v != 0xff000000u;
					}
				}
			}
			say("VKTEST: part 5: pixel (8,8) %08x (56,56) %08x; green %u black %u other %u; %u clearly wrong%s",
			    p[8 * 64 + 8], p[56 * 64 + 56], green, black_px, other, bad, bad ? " FAIL" : "");
			for (unsigned y = 0; y < 64; y += 8) {
				char row[65];
				for (unsigned x = 0; x < 64; x++) {
					uint32_t v = p[y * 64 + x];
					row[x] = v == 0xff00ff00u ? '#' : v == 0xff000000u ? '.' : '?';
				}
				row[64] = 0;
				say("VKTEST: part 5: row %2u %s", y, row);
			}
			failures += bad != 0;
		}
	}
done:
	say("VKTEST: %s (%u failures)", failures ? "FAIL" : "PASS", failures);
	/* The Metal test, when installed (one sweetexecswap starts vktest; mtltest follows it). */
	if (access(MTLTEST, X_OK) == 0) {
		char *argv[] = { "mtltest", NULL };
		pid_t pid;
		int status = 0;
		int e = posix_spawn(&pid, MTLTEST, NULL, NULL, argv, environ);
		if (!e) {
			waitpid(pid, &status, 0);
		}
		say("VKTEST: %s -> spawn %d, exit status %#x", MTLTEST, e, status);
	}
	return failures != 0;
}
