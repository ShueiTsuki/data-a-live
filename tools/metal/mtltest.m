/*
 * mtltest: iOS Metal on the Adreno 840 through UniXNU's NativeMetal plugin (adapted by
 * adapt_ios16.py), the way an app reaches it: MTLCreateSystemDefaultDevice() runs Metal's own
 * discovery (IOAcceleratorES -> MetalPluginName -> NativeAdrenoMetal.bundle), and the plugin hands
 * a device only to processes named in NativeAdrenoMetal.clients. Three parts, logged on their own:
 *   1. the device (name, family support)
 *   2. compute: mtltest.metal's fill kernel over 4096 threads into a shared buffer, read back
 *   3. render: a triangle into a 64x64 RGBA8 texture, blitted to a buffer and checked
 *   4. IOSurface: the same triangle into a texture over an IOSurface (BoImport), read by the CPU
 * Lines go to /dev/console with the prefix "MTLTEST:" (the kernel log); stdout and stderr too.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <IOSurface/IOSurfaceRef.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#ifdef MTLTEST_AIR
#include "mtltest_air.h"   /* mtltest.air.ll assembled by llvm-as, as bytes (xxd -i), made by build_plugin_linux.sh */
#else
#include "mtltest_metallib.h"   /* mtltest.metallib as bytes (xxd -i), made by build_plugin.sh */
#endif

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

static bool
check(const char *what, id object, NSError *error)
{
	say("MTLTEST: %s -> %s%s%s", what, object ? "ok" : "nil",
	    error ? " " : "", error ? error.localizedDescription.UTF8String : "");
	failures += object == nil;
	return object == nil;
}

/* ---- part 7: the sweep (bb36) ----
 * Every pairing of format x size x upload path x sampler draws a texture of four quadrant colours (red, green / blue,
 * white; for block-compressed formats the quadrants follow whole blocks) into a 64x64 RGBA8 target; 16 points away
 * from the quadrant edges are compared with what the format holds, and transparent pixels are counted. Then 16 1x1
 * textures of distinct colours at fragment indices 0-15, each read back through its index. Once per boot (launchd
 * restarts mtltest with vktest): the boot session's UUID is kept in /var/tmp/mtltest.sweep. */
enum { SWEEP_W = 64 };
static const float kQuadrant[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };

static unsigned
quadrantOf(NSUInteger x, NSUInteger y, NSUInteger w, NSUInteger h, NSUInteger bw, NSUInteger bh)
{
	const NSUInteger bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
	return (x / bw >= bx / 2 ? 1 : 0) + (y / bh >= by / 2 ? 2 : 0);
}

static uint16_t
half1(float v)
{
	return v > 0.5f ? 0x3c00 : 0;
}

/* The texel bytes of `format` for colour c; false for a format the sweep does not make. Block formats: one LDR
 * void-extent block. `channels` says which of r, g, b, a the format keeps (others read as 0, 0, 0, 1). */
static bool
encode(MTLPixelFormat format, const float c[4], uint8_t *out, NSUInteger *bytes, NSUInteger *bw, NSUInteger *bh,
    unsigned *channels)
{
	const uint8_t r = c[0] > 0.5f ? 255 : 0, g = c[1] > 0.5f ? 255 : 0, b = c[2] > 0.5f ? 255 : 0, a = c[3] > 0.5f ? 255 : 0;
	*bw = *bh = 1;
	*channels = 15;
	switch (format) {
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
		out[0] = b, out[1] = g, out[2] = r, out[3] = a, *bytes = 4;
		return true;
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
		out[0] = r, out[1] = g, out[2] = b, out[3] = a, *bytes = 4;
		return true;
	case MTLPixelFormatR8Unorm:
		out[0] = r, *bytes = 1, *channels = 1;
		return true;
	case MTLPixelFormatRG8Unorm:
		out[0] = r, out[1] = g, *bytes = 2, *channels = 3;
		return true;
	case MTLPixelFormatRGBA16Float: {
		const uint16_t h[4] = { half1(c[0]), half1(c[1]), half1(c[2]), half1(c[3]) };
		memcpy(out, h, 8), *bytes = 8;
		return true;
	}
	case MTLPixelFormatRGB10A2Unorm: {
		const uint32_t v = (r ? 1023u : 0) | (g ? 1023u : 0) << 10 | (b ? 1023u : 0) << 20 | (a ? 3u : 0) << 30;
		memcpy(out, &v, 4), *bytes = 4;
		return true;
	}
	case MTLPixelFormatASTC_4x4_LDR:
	case MTLPixelFormatASTC_4x4_sRGB:
	case MTLPixelFormatASTC_8x8_LDR: {
		const uint8_t head[8] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		const uint16_t v[4] = { r ? 0xffff : 0, g ? 0xffff : 0, b ? 0xffff : 0, a ? 0xffff : 0 };
		memcpy(out, head, 8), memcpy(out + 8, v, 8), *bytes = 16;
		*bw = *bh = format == MTLPixelFormatASTC_8x8_LDR ? 8 : 4;
		return true;
	}
	default:
		return false;
	}
}

static bool
sweepOncePerBoot(void)
{
	char uuid[64] = { 0 }, seen[64] = { 0 };
	size_t size = sizeof(uuid) - 1;
	if (sysctlbyname("kern.bootsessionuuid", uuid, &size, NULL, 0) != 0) {
		return true;
	}
	int fd = open("/var/tmp/mtltest.sweep", O_RDONLY);
	if (fd >= 0) {
		(void)read(fd, seen, sizeof(seen) - 1);
		close(fd);
		if (!strcmp(seen, uuid)) {
			return false;
		}
	}
	fd = open("/var/tmp/mtltest.sweep", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		(void)write(fd, uuid, strlen(uuid));
		close(fd);
	}
	return true;
}

static void
sweep(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib)
{
	if (!sweepOncePerBoot()) {
		say("MTLTEST: part 7: sweep already ran this boot");
		return;
	}
	NSError *err = nil;
	MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
	rpd.vertexFunction = [lib newFunctionWithName:@"quad_vs"];
	rpd.fragmentFunction = [lib newFunctionWithName:@"sweep_fs"];
	rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
	MTLTextureDescriptor *rt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	    width:SWEEP_W height:SWEEP_W mipmapped:NO];
	rt.usage = MTLTextureUsageRenderTarget;
	rt.storageMode = MTLStorageModePrivate;
	id<MTLTexture> target = [dev newTextureWithDescriptor:rt];
	id<MTLBuffer> out = [dev newBufferWithLength:SWEEP_W * SWEEP_W * 4 options:MTLResourceStorageModeShared];
	if (check("part 7: sweep pipeline", rps, err) || check("part 7: target", target, nil)) {
		return;
	}
	/* samplers: 0 nearest, 1 linear, 2 linear in pixel coordinates with clamp-to-zero, 3 nearest with repeat */
	id<MTLSamplerState> samplers[4];
	const char *samplerName[4] = { "nearest", "linear", "pixel", "repeat" };
	for (unsigned s = 0; s < 4; s++) {
		MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
		sd.minFilter = sd.magFilter = (s == 1 || s == 2) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		sd.sAddressMode = sd.tAddressMode = s == 2 ? MTLSamplerAddressModeClampToZero :
		    s == 3 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
		sd.normalizedCoordinates = s != 2;
		samplers[s] = [dev newSamplerStateWithDescriptor:sd];
		if (check("part 7: sampler", samplers[s], nil)) {
			return;
		}
	}
	const MTLPixelFormat formats[] = { MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm,
		MTLPixelFormatRGBA16Float, MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm, MTLPixelFormatRGB10A2Unorm,
		MTLPixelFormatASTC_4x4_LDR, MTLPixelFormatASTC_4x4_sRGB, MTLPixelFormatASTC_8x8_LDR };
	const NSUInteger sizes[][2] = { { 64, 64 }, { 333, 77 }, { 1125, 2436 } };   /* bb36: 2048x2048 ran the shared pool out */
	const char *uploadName[4] = { "replace", "blit-private", "nocopy", "iosurface" };
	unsigned cases = 0, good = 0, told = 0;
	for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		if ([(id)dev respondsToSelector:@selector(currentAllocatedSize)]) {   /* bb36: is what the sweep frees given back? */
			say("MTLTEST: part 7: before format %lu the device holds %lu MiB", (unsigned long)formats[f],
			    (unsigned long)(dev.currentAllocatedSize >> 20));
		}
		for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
			for (unsigned u = 0; u < 4; u++) @autoreleasepool {
				const MTLPixelFormat format = formats[f];
				const NSUInteger w = sizes[z][0], h = sizes[z][1];
				uint8_t texel[4][16];
				NSUInteger tb = 0, bw = 1, bh = 1;
				unsigned channels = 15;
				for (unsigned q = 0; q < 4; q++) {
					encode(format, kQuadrant[q], texel[q], &tb, &bw, &bh, &channels);
				}
				if (u == 3 && format != MTLPixelFormatBGRA8Unorm) {
					continue;   /* IOSurface: BGRA only */
				}
				const NSUInteger cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh, row = cols * tb, length = row * rows;
				MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h
				    mipmapped:NO];
				td.usage = MTLTextureUsageShaderRead;
				id<MTLTexture> tex = nil;
				IOSurfaceRef surface = NULL;
				void *mem = NULL;
				NSUInteger memLength = (length + 16383) & ~(NSUInteger)16383;
				uint8_t *bytes = malloc(length);
				for (NSUInteger y = 0; y < rows; y++) {
					for (NSUInteger x = 0; x < cols; x++) {
						memcpy(bytes + y * row + x * tb, texel[quadrantOf(x * bw, y * bh, w, h, bw, bh)], tb);
					}
				}
				if (u == 0) {
					tex = [dev newTextureWithDescriptor:td];
					[tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
				} else if (u == 1) {
					td.storageMode = MTLStorageModePrivate;
					tex = [dev newTextureWithDescriptor:td];
					id<MTLBuffer> up = [dev newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
					[bl copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:length
					    sourceSize:MTLSizeMake(w, h, 1) toTexture:tex destinationSlice:0 destinationLevel:0
					    destinationOrigin:MTLOriginMake(0, 0, 0)];
					[bl endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
				} else if (u == 2) {
					SEL sel = NSSelectorFromString(@"newTextureWithBytesNoCopy:length:descriptor:deallocator:");
					if ([(id)dev respondsToSelector:sel] && posix_memalign(&mem, 16384, memLength) == 0) {
						memset(mem, 0, memLength);
						memcpy(mem, bytes, length);
						id (*make)(id, SEL, void *, NSUInteger, MTLTextureDescriptor *, void (^)(void *, NSUInteger)) =
						    (id (*)(id, SEL, void *, NSUInteger, MTLTextureDescriptor *, void (^)(void *, NSUInteger)))objc_msgSend;
						tex = make(dev, sel, mem, memLength, td, ^(void *p, NSUInteger l) { (void)l; free(p); });
						if (!tex) {
							free(mem);
						}
					}
				} else {
					const int pf = 'BGRA', bpe = 4;
					NSDictionary *props = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
						(id)kIOSurfaceBytesPerElement: @(bpe), (id)kIOSurfacePixelFormat: @(pf) };
					surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
					if (surface) {
						IOSurfaceLock(surface, 0, NULL);
						uint8_t *base = IOSurfaceGetBaseAddress(surface);
						const size_t stride = IOSurfaceGetBytesPerRow(surface);
						for (NSUInteger y = 0; y < h; y++) {
							memcpy(base + y * stride, bytes + y * row, row);
						}
						IOSurfaceUnlock(surface, 0, NULL);
						tex = [dev newTextureWithDescriptor:td iosurface:surface plane:0];
					}
				}
				free(bytes);
				cases++;
				if (!tex) {
					say("MTLTEST: part 7: %lu %lux%lu %s: no texture", (unsigned long)format, (unsigned long)w, (unsigned long)h,
					    uploadName[u]);
					if (surface) {
						CFRelease(surface);
					}
					continue;
				}
				char result[160] = "";
				bool allGood = true;
				for (unsigned s = 0; s < 4; s++) {
					const float scale[2] = { s == 2 ? (float)w : 1.0f, s == 2 ? (float)h : 1.0f };
					MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = target;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
					[enc setRenderPipelineState:rps];
					[enc setFragmentTexture:tex atIndex:0];
					[enc setFragmentSamplerState:samplers[s] atIndex:0];
					[enc setFragmentBytes:scale length:sizeof(scale) atIndex:0];
					[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[enc endEncoding];
					id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
					[bl copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
					    sourceSize:MTLSizeMake(SWEEP_W, SWEEP_W, 1) toBuffer:out destinationOffset:0
					    destinationBytesPerRow:SWEEP_W * 4 destinationBytesPerImage:SWEEP_W * SWEEP_W * 4];
					[bl endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					const uint8_t *px = (const uint8_t *)out.contents;
					unsigned bad = 0, clear = 0;
					for (unsigned i = 0; i < SWEEP_W * SWEEP_W; i++) {
						clear += px[i * 4 + 3] == 0;
					}
					static const unsigned points[4] = { 8, 24, 40, 56 };
					for (unsigned pj = 0; pj < 4; pj++) {
						for (unsigned pi = 0; pi < 4; pi++) {
							const unsigned i = points[pi], j = points[pj];
							const NSUInteger tx = (NSUInteger)((i + 0.5) / SWEEP_W * w), ty = (NSUInteger)((j + 0.5) / SWEEP_W * h);
							const float *c = kQuadrant[quadrantOf(tx, ty, w, h, bw, bh)];
							const uint8_t *p = px + (j * SWEEP_W + i) * 4;
							for (unsigned k = 0; k < 4; k++) {
								const int want = (channels >> k & 1) ? (c[k] > 0.5f ? 255 : 0) : (k == 3 ? 255 : 0);
								if (abs((int)p[k] - want) > 3) {
									bad++;
									break;
								}
							}
						}
					}
					const bool ok = bad == 0 && clear == 0 && cb.status == MTLCommandBufferStatusCompleted;
					allGood &= ok;
					if (!ok) {
						const size_t used = strlen(result);
						snprintf(result + used, sizeof(result) - used, " %s: %u of 16 wrong, %u clear, status %ld, centre %02x%02x%02x%02x;",
						    samplerName[s], bad, clear, (long)cb.status, px[(32 * SWEEP_W + 16) * 4], px[(32 * SWEEP_W + 16) * 4 + 1],
						    px[(32 * SWEEP_W + 16) * 4 + 2], px[(32 * SWEEP_W + 16) * 4 + 3]);
					}
				}
				good += allGood;
				if (!allGood && told++ < 150) {
					say("MTLTEST: part 7: %lu %lux%lu %s FAIL:%s", (unsigned long)format, (unsigned long)w, (unsigned long)h,
					    uploadName[u], result);
				}
				failures += !allGood;
				tex = nil;
				if (surface) {
					CFRelease(surface);
				}
			}
		}
	}
	say("MTLTEST: part 7: sweep %u of %u textures right with all 4 samplers", good, cases);

	/* fragment texture indices 0-15 */
	rpd.fragmentFunction = [lib newFunctionWithName:@"index_fs"];
	err = nil;
	id<MTLRenderPipelineState> ips = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
	if (check("part 7: index pipeline", ips, err)) {
		return;
	}
	id<MTLTexture> one[16];
	for (unsigned k = 0; k < 16; k++) {
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1
		    height:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		one[k] = [dev newTextureWithDescriptor:d];
		const uint8_t c[4] = { (uint8_t)(k * 16 + 8), (uint8_t)(255 - k * 16), (uint8_t)(k * 8), 255 };
		[one[k] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:c bytesPerRow:4];
	}
	char wrong[256] = "";
	unsigned right = 0;
	for (uint32_t which = 0; which < 16; which++) {
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
		[enc setRenderPipelineState:ips];
		for (unsigned k = 0; k < 16; k++) {
			[enc setFragmentTexture:one[k] atIndex:k];
		}
		[enc setFragmentBytes:&which length:sizeof(which) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
		[bl copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
		    toBuffer:out destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
		[bl endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		const uint8_t *p = (const uint8_t *)out.contents;
		if (p[0] == (uint8_t)(which * 16 + 8) && p[1] == (uint8_t)(255 - which * 16) && p[2] == (uint8_t)(which * 8)) {
			right++;
		} else {
			const size_t used = strlen(wrong);
			snprintf(wrong + used, sizeof(wrong) - used, " %u->%02x%02x%02x", which, p[0], p[1], p[2]);
		}
	}
	say("MTLTEST: part 7: texture indices %u of 16 right%s%s", right, right == 16 ? "" : " FAIL:", wrong);
	failures += right != 16;
}

/* ---- part 8: framebuffer fetch (bb37) ----
 * QuartzCore's App Library draw reads its attachments in the shader (fc_framebuffer_fetch) and blends there; its
 * output came out transparent although the texture and its sampling are right (bb30-bb36). Each case draws into
 * BGRA8 targets of 64x64 and of 1125x2436 (private, as QuartzCore's) and checks five points:
 *   a: cleared red, fetch_fs -> blue          b: solid_fs then fetch_fs in one pass -> solid with red and blue swapped
 *   c: filled red by an earlier pass, loaded -> blue
 *   d: cleared white, fetch_tex_fs over an all-red ASTC 1125x2436 image -> red
 *   e: two attachments cleared red and blue, fetch2_fs -> swapped
 * mtltest runs again and again in a boot (launchd restarts it with vktest); each launch takes the next TU_DEBUG of
 * kTuDebug (main), so one boot shows the cases under each. */
static const char *const kTuDebug[] = { "sysmem,nocb", "sysmem,nocb,noubwc", "sysmem,nocb,flushall", "sysmem,nocb,rast_order",
	"sysmem,nocb,syncdraw", "nocb", "sysmem,nocb,noubwc,flushall" };

static void
fetchTests(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib)
{
	const char *tu = getenv("TU_DEBUG") ? getenv("TU_DEBUG") : "-";
	id<MTLRenderPipelineState> (^pipeline)(NSString *, unsigned) = ^id<MTLRenderPipelineState>(NSString *fs, unsigned colors) {
		MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
		d.vertexFunction = [lib newFunctionWithName:@"quad_vs"];
		d.fragmentFunction = [lib newFunctionWithName:fs];
		for (unsigned c = 0; c < colors; c++) {
			d.colorAttachments[c].pixelFormat = MTLPixelFormatBGRA8Unorm;
		}
		NSError *e = nil;
		id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
		if (!p) {
			say("MTLTEST: part 8 [%s]: pipeline %s refused: %s", tu, fs.UTF8String, e.localizedDescription.UTF8String);
		}
		return p;
	};
	id<MTLRenderPipelineState> solid = pipeline(@"solid_fs", 1), fetch = pipeline(@"fetch_fs", 1),
	    fetchTex = pipeline(@"fetch_tex_fs", 1), fetch2 = pipeline(@"fetch2_fs", 2);
	/* the all-red ASTC image */
	static const uint8_t red[16] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff };
	MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:1125
	    height:2436 mipmapped:NO];
	ad.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> astc = [dev newTextureWithDescriptor:ad];
	{
		const NSUInteger row = 282 * 16, rows = 609;
		uint8_t *bytes = malloc(row * rows);
		for (NSUInteger i = 0; i < row * rows; i += 16) {
			memcpy(bytes + i, red, 16);
		}
		[astc replaceRegion:MTLRegionMake2D(0, 0, 1125, 2436) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
		free(bytes);
	}
	const NSUInteger sizes[2][2] = { { 64, 64 }, { 1125, 2436 } };
	for (unsigned z = 0; z < 2; z++) @autoreleasepool {
		const NSUInteger w = sizes[z][0], h = sizes[z][1];
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w
		    height:h mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		id<MTLTexture> t0 = [dev newTextureWithDescriptor:td], t1 = [dev newTextureWithDescriptor:td];
		id<MTLBuffer> out = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
		if (!t0 || !t1 || !out) {
			say("MTLTEST: part 8 [%s] %lux%lu: no target", tu, (unsigned long)w, (unsigned long)h);
			continue;
		}
		/* one pass over t0 (and t1 when `two`), loaded or cleared, drawing `draws` in order; then t0 (or t1) read back */
		void (^pass)(MTLLoadAction, MTLClearColor, MTLClearColor, NSArray *, id<MTLTexture>, bool) =
		    ^(MTLLoadAction load, MTLClearColor c0, MTLClearColor c1, NSArray *draws, id<MTLTexture> tex, bool two) {
			MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
			p.colorAttachments[0].texture = t0;
			p.colorAttachments[0].loadAction = load;
			p.colorAttachments[0].clearColor = c0;
			p.colorAttachments[0].storeAction = MTLStoreActionStore;
			if (two) {
				p.colorAttachments[1].texture = t1;
				p.colorAttachments[1].loadAction = load;
				p.colorAttachments[1].clearColor = c1;
				p.colorAttachments[1].storeAction = MTLStoreActionStore;
			}
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:p];
			for (id<MTLRenderPipelineState> state in draws) {
				[enc setRenderPipelineState:state];
				if (tex) {
					[enc setFragmentTexture:tex atIndex:0];
				}
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			}
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
		};
		/* the colour at five points of `t` as BGRA bytes, compared with `want`; a result line part */
		NSString *(^check8)(id<MTLTexture>, const uint8_t *) = ^NSString *(id<MTLTexture> t, const uint8_t *want) {
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
			[bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
			    toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
			[bl endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			const uint8_t *px = (const uint8_t *)out.contents;
			const NSUInteger points[5][2] = { { w / 2, h / 2 }, { w / 4, h / 4 }, { 3 * w / 4, h / 4 }, { w / 4, 3 * h / 4 },
				{ 3 * w / 4, 3 * h / 4 } };
			unsigned bad = 0;
			for (unsigned k = 0; k < 5; k++) {
				const uint8_t *p = px + (points[k][1] * w + points[k][0]) * 4;
				for (unsigned c = 0; c < 4; c++) {
					if (abs((int)p[c] - want[c]) > 3) {
						bad++;
						break;
					}
				}
			}
			const uint8_t *m = px + (h / 2 * w + w / 2) * 4;
			return bad ? [NSString stringWithFormat:@"FAIL %u/5 (centre bgra %02x%02x%02x%02x, want %02x%02x%02x%02x)", bad, m[0],
			    m[1], m[2], m[3], want[0], want[1], want[2], want[3]] : @"ok";
		};
		const MTLClearColor redC = MTLClearColorMake(1, 0, 0, 1), blueC = MTLClearColorMake(0, 0, 1, 1),
		    blackC = MTLClearColorMake(0, 0, 0, 1), whiteC = MTLClearColorMake(1, 1, 1, 1);
		static const uint8_t blue[4] = { 255, 0, 0, 255 }, redB[4] = { 0, 0, 255, 255 }, swapped[4] = { 64, 128, 191, 255 };
		NSMutableString *line = [NSMutableString string];
		if (fetch) {
			pass(MTLLoadActionClear, redC, redC, @[ fetch ], nil, false);
			[line appendFormat:@" a %@;", check8(t0, blue)];
		}
		if (solid && fetch) {
			pass(MTLLoadActionClear, blackC, blackC, @[ solid, fetch ], nil, false);
			[line appendFormat:@" b %@;", check8(t0, swapped)];
		}
		if (fetch && solid) {
			pass(MTLLoadActionClear, redC, redC, @[], nil, false);
			pass(MTLLoadActionLoad, redC, redC, @[ fetch ], nil, false);
			[line appendFormat:@" c %@;", check8(t0, blue)];
		}
		if (fetchTex && astc) {
			pass(MTLLoadActionClear, whiteC, whiteC, @[ fetchTex ], astc, false);
			[line appendFormat:@" d %@;", check8(t0, redB)];
		}
		if (fetch2) {
			pass(MTLLoadActionClear, redC, blueC, @[ fetch2 ], nil, true);
			[line appendFormat:@" e c0 %@, c1 %@;", check8(t0, blue), check8(t1, redB)];
		}
		say("MTLTEST: part 8 [TU_DEBUG=%s] %lux%lu:%s", tu, (unsigned long)w, (unsigned long)h, line.UTF8String);
		failures += [line containsString:@"FAIL"];
	}
}

/* Each launch in a boot takes the next of kTuDebug (counted in /var/tmp/mtltest.launch with the boot session's UUID). */
static void
pickTuDebug(void)
{
	char uuid[64] = { 0 }, text[128] = { 0 };
	size_t size = sizeof(uuid) - 1;
	unsigned n = 0;
	if (sysctlbyname("kern.bootsessionuuid", uuid, &size, NULL, 0) != 0) {
		return;
	}
	int fd = open("/var/tmp/mtltest.launch", O_RDONLY);
	if (fd >= 0) {
		(void)read(fd, text, sizeof(text) - 1);
		close(fd);
		char *space = strchr(text, ' ');
		if (space && (size_t)(space - text) == strlen(uuid) && !strncmp(text, uuid, strlen(uuid))) {
			n = (unsigned)atoi(space + 1);
		}
	}
	fd = open("/var/tmp/mtltest.launch", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		snprintf(text, sizeof(text), "%s %u", uuid, n + 1);
		(void)write(fd, text, strlen(text));
		close(fd);
	}
	setenv("TU_DEBUG", kTuDebug[n % (sizeof(kTuDebug) / sizeof(kTuDebug[0]))], 1);
}

static void
run(void)
{
		/* ---- part 1: the device ---- */
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (check("part 1: MTLCreateSystemDefaultDevice", dev, nil)) {
			return;
		}
		say("MTLTEST: device '%s' class %s apple3 %d common2 %d", dev.name.UTF8String,
		    NSStringFromClass([(id)dev class]).UTF8String, [dev supportsFamily:MTLGPUFamilyApple3],
		    [dev supportsFamily:MTLGPUFamilyCommon2]);
		NSError *err = nil;
#ifdef MTLTEST_AIR
		/* Built without Apple's metal compiler: bare AIR goes to the plugin's own entry for it, found through the image
		 * that implements the device's methods. */
		id (*withAir)(const void *, size_t) = NULL;
		Dl_info info = { 0 };
		IMP imp = class_getMethodImplementation([(id)dev class], @selector(newCommandQueue));
		if (dladdr((const void *)imp, &info) && info.dli_fname) {
			void *image = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_LAZY);
			if (image) {
				withAir = (id (*)(const void *, size_t))dlsym(image, "native_bridge_metal_library_with_air");
			}
		}
		id<MTLLibrary> lib = withAir ? withAir(mtltest_air, mtltest_air_len) : nil;
		if (check("part 1: library from AIR (native_bridge_metal_library_with_air)", lib, nil)) {
			return;
		}
#else
		dispatch_data_t data = dispatch_data_create(mtltest_metallib, mtltest_metallib_len, NULL,
		    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		id<MTLLibrary> lib = [dev newLibraryWithData:data error:&err];
		if (check("part 1: newLibraryWithData", lib, err)) {
			return;
		}
#endif
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		if (check("part 1: newCommandQueue", queue, nil)) {
			return;
		}
		id<MTLBuffer> buf = [dev newBufferWithLength:64 * 1024 options:MTLResourceStorageModeShared];
		if (check("part 1: newBufferWithLength", buf, nil)) {
			return;
		}
		volatile uint32_t *p = (volatile uint32_t *)buf.contents;

		/* ---- part 2: compute ---- */
		{
			id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
			err = nil;
			id<MTLComputePipelineState> cps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
			if (!check("part 2: compute pipeline (translated and compiled)", cps, err)) {
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
				[enc setComputePipelineState:cps];
				[enc setBuffer:buf offset:0 atIndex:0];
				[enc dispatchThreadgroups:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[enc endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				unsigned bad = 0;
				for (unsigned i = 0; i < 4096; i++) {
					bad += p[i] != i * 3u + 0x1000u;
				}
				say("MTLTEST: part 2: status %ld, words %08x %08x ... %08x, %u of 4096 wrong%s", (long)cb.status,
				    p[0], p[1], p[4095], bad, bad ? " FAIL" : "");
				failures += bad != 0 || cb.status != MTLCommandBufferStatusCompleted;
			}
		}

		/* ---- part 3: render ---- */
		{
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			    width:64 height:64 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget;
			td.storageMode = MTLStorageModePrivate;
			id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
			MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
			rpd.vertexFunction = [lib newFunctionWithName:@"tri_vs"];
			rpd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
			rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			err = nil;
			id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
			if (!check("part 3: texture", tex, nil) &&
			    !check("part 3: render pipeline (translated and compiled)", rps, err)) {
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = tex;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:rps];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				    sourceSize:MTLSizeMake(64, 64, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:256
				    destinationBytesPerImage:256 * 64];
				[blit endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				unsigned green = 0, black = 0, other = 0, bad = 0;
				for (unsigned y = 0; y < 64; y++) {
					for (unsigned x = 0; x < 64; x++) {
						uint32_t v = p[y * 64 + x];
						green += v == 0xff00ff00u;
						black += v == 0xff000000u;
						other += v != 0xff00ff00u && v != 0xff000000u;
						if (x + y + 2 < 63) {
							bad += v != 0xff00ff00u;
						} else if (x + y > 63 + 2) {
							bad += v != 0xff000000u;
						}
					}
				}
				say("MTLTEST: part 3: status %ld; green %u black %u other %u; %u clearly wrong%s", (long)cb.status,
				    green, black, other, bad, bad ? " FAIL" : "");
				for (unsigned y = 0; y < 64; y += 8) {
					char row[65];
					for (unsigned x = 0; x < 64; x++) {
						uint32_t v = p[y * 64 + x];
						row[x] = v == 0xff00ff00u ? '#' : v == 0xff000000u ? '.' : '?';
					}
					row[64] = 0;
					say("MTLTEST: part 3: row %2u %s", y, row);
				}
				failures += bad != 0 || cb.status != MTLCommandBufferStatusCompleted;
			}
		}

		/* ---- part 4: render into an IOSurface (the render server's way), read by the CPU ---- */
		{
			NSDictionary *props = @{
				(id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64, (id)kIOSurfaceBytesPerElement: @4,
				(id)kIOSurfaceBytesPerRow: @256, (id)kIOSurfacePixelFormat: @((uint32_t)'RGBA'),
			};
			IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			    width:64 height:64 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModeShared;
			id<MTLTexture> tex = surface ? [dev newTextureWithDescriptor:td iosurface:surface plane:0] : nil;
			MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
			rpd.vertexFunction = [lib newFunctionWithName:@"tri_vs"];
			rpd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
			rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			err = nil;
			id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
			say("MTLTEST: part 4: IOSurface %p id %u", surface, surface ? IOSurfaceGetID(surface) : 0);
			if (!check("part 4: texture over the IOSurface (BoImport)", tex, nil) &&
			    !check("part 4: render pipeline", rps, err)) {
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = tex;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:rps];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
				const volatile uint32_t *s = (const volatile uint32_t *)IOSurfaceGetBaseAddress(surface);
				const size_t row = IOSurfaceGetBytesPerRow(surface) / 4;
				unsigned green = 0, black = 0, other = 0, bad = 0;
				for (unsigned y = 0; y < 64; y++) {
					for (unsigned x = 0; x < 64; x++) {
						uint32_t v = s[y * row + x];
						green += v == 0xff00ff00u;
						black += v == 0xff000000u;
						other += v != 0xff00ff00u && v != 0xff000000u;
						if (x + y + 2 < 63) {
							bad += v != 0xff00ff00u;
						} else if (x + y > 63 + 2) {
							bad += v != 0xff000000u;
						}
					}
				}
				say("MTLTEST: part 4: status %ld row %zu; green %u black %u other %u; %u clearly wrong%s",
				    (long)cb.status, row * 4, green, black, other, bad, bad ? " FAIL" : "");
				for (unsigned y = 0; y < 64; y += 16) {
					char line[65];
					for (unsigned x = 0; x < 64; x++) {
						uint32_t v = s[y * row + x];
						line[x] = v == 0xff00ff00u ? '#' : v == 0xff000000u ? '.' : '?';
					}
					line[64] = 0;
					say("MTLTEST: part 4: row %2u %s", y, line);
				}
				IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
				failures += bad != 0 || cb.status != MTLCommandBufferStatusCompleted;
			}
			if (surface) {
				CFRelease(surface);
			}
		}

		/* ---- part 5: sample an ASTC 4x4 image (the App Library's material, bb27-bb29) ---- */
		/* 8x8 texels = 2x2 void-extent blocks, one colour each: red, green (top), blue, white (bottom).
		 * (a) an ordinary texture filled by replaceRegion, (b) newTextureWithBytesNoCopy as QuartzCore makes it. */
		{
			uint8_t blocks[4][16];
			const uint16_t colour[4][4] = { { 0xffff, 0, 0, 0xffff }, { 0, 0xffff, 0, 0xffff },
				{ 0, 0, 0xffff, 0xffff }, { 0xffff, 0xffff, 0xffff, 0xffff } };
			for (unsigned b = 0; b < 4; b++) {
				const uint8_t head[8] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };   /* LDR void extent, no extent */
				memcpy(blocks[b], head, 8);
				memcpy(blocks[b] + 8, colour[b], 8);
			}
			const uint32_t want[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
			MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
			rpd.vertexFunction = [lib newFunctionWithName:@"quad_vs"];
			rpd.fragmentFunction = [lib newFunctionWithName:@"tex_fs"];
			rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			err = nil;
			id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
			MTLTextureDescriptor *rt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			    width:8 height:8 mipmapped:NO];
			rt.usage = MTLTextureUsageRenderTarget;
			rt.storageMode = MTLStorageModePrivate;
			id<MTLTexture> target = [dev newTextureWithDescriptor:rt];
			MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR
			    width:8 height:8 mipmapped:NO];
			ad.usage = MTLTextureUsageShaderRead;
			ad.storageMode = MTLStorageModeShared;
			for (unsigned way = 0; way < 2 && !check("part 5: render pipeline", rps, err) && !check("part 5: target", target, nil);
			    way++) {
				id<MTLTexture> astc = nil;
				void *bytes = NULL;
				if (way == 0) {
					astc = [dev newTextureWithDescriptor:ad];
					[astc replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:blocks bytesPerRow:32];
				} else if (posix_memalign(&bytes, 16384, 16384) == 0) {
					memset(bytes, 0, 16384);
					memcpy(bytes, blocks, sizeof(blocks));
					SEL sel = NSSelectorFromString(@"newTextureWithBytesNoCopy:length:descriptor:deallocator:");
					if ([(id)dev respondsToSelector:sel]) {
						id (*make)(id, SEL, void *, NSUInteger, MTLTextureDescriptor *, void (^)(void *, NSUInteger)) =
						    (id (*)(id, SEL, void *, NSUInteger, MTLTextureDescriptor *, void (^)(void *, NSUInteger)))objc_msgSend;
						astc = make(dev, sel, bytes, 16384, ad, ^(void *b, NSUInteger l) { (void)l; free(b); });
					} else {
						free(bytes);
					}
				}
				if (check(way ? "part 5b: ASTC texture (newTextureWithBytesNoCopy)" : "part 5a: ASTC texture (replaceRegion)",
				    astc, nil)) {
					continue;
				}
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = target;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:rps];
				[enc setFragmentTexture:astc atIndex:0];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				    sourceSize:MTLSizeMake(8, 8, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:32
				    destinationBytesPerImage:256];
				[blit endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				const uint32_t *px = (const uint32_t *)buf.contents;
				unsigned bad = 0;
				for (unsigned y = 0; y < 8; y++) {
					for (unsigned x = 0; x < 8; x++) {
						bad += px[y * 8 + x] != want[(y / 4) * 2 + x / 4];
					}
				}
				say("MTLTEST: part 5%c: status %ld; corners %08x %08x %08x %08x (want %08x %08x %08x %08x); %u of 64 wrong%s",
				    way ? 'b' : 'a', (long)cb.status, px[0], px[7], px[56], px[63], want[0], want[1], want[2], want[3], bad,
				    bad ? " FAIL" : "");
				failures += bad != 0 || cb.status != MTLCommandBufferStatusCompleted;
			}
		}

		/* ---- part 6: what differs between part 5 and the App Library (bb32-bb35) ---- */
		/* QuartzCore's 1125x2436 ASTC images hold the right blocks (read back), yet sampling them draws transparent black.
		 * Each case draws an all-red image into a 64x64 target and counts red pixels: (a) ASTC 1125x2436, (b) ASTC 64x64,
		 * (c) BGRA8 1125x2436, (d) ASTC 1125x2436 with linear filtering, (e) ASTC 1125x2436 in pixel coordinates with
		 * clamp-to-zero, (f) ASTC 1125x2436 bound at texture index 3 as QuartzCore binds it. */
		{
			static const uint8_t red[16] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff };
			id<MTLBuffer> out = [dev newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
			MTLTextureDescriptor *rt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			    width:64 height:64 mipmapped:NO];
			rt.usage = MTLTextureUsageRenderTarget;
			rt.storageMode = MTLStorageModePrivate;
			id<MTLTexture> target = [dev newTextureWithDescriptor:rt];
			id<MTLTexture> (^astcTexture)(NSUInteger, NSUInteger) = ^id<MTLTexture>(NSUInteger w, NSUInteger h) {
				const NSUInteger row = (w + 3) / 4 * 16, rows = (h + 3) / 4;
				uint8_t *bytes = malloc(row * rows);
				for (NSUInteger i = 0; i < row * rows; i += 16) {
					memcpy(bytes + i, red, 16);
				}
				MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR
				    width:w height:h mipmapped:NO];
				d.usage = MTLTextureUsageShaderRead;
				id<MTLTexture> t = [dev newTextureWithDescriptor:d];
				[t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
				free(bytes);
				return t;
			};
			MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			    width:1125 height:2436 mipmapped:NO];
			pd.usage = MTLTextureUsageShaderRead;
			id<MTLTexture> plain = [dev newTextureWithDescriptor:pd];
			uint32_t *px = malloc(1125 * 2436 * 4);
			for (NSUInteger i = 0; i < 1125 * 2436; i++) {
				px[i] = 0xffff0000u;   /* BGRA8: red */
			}
			[plain replaceRegion:MTLRegionMake2D(0, 0, 1125, 2436) mipmapLevel:0 withBytes:px bytesPerRow:1125 * 4];
			free(px);
			id<MTLTexture> big = astcTexture(1125, 2436), small = astcTexture(64, 64);
			struct { const char *name, *fs; id<MTLTexture> tex; NSUInteger index; } cases[] = {
				{ "6a ASTC 1125x2436 nearest", "tex_fs", big, 0 },
				{ "6b ASTC 64x64 nearest", "tex_fs", small, 0 },
				{ "6c BGRA8 1125x2436 nearest", "tex_fs", plain, 0 },
				{ "6d ASTC 1125x2436 linear", "tex_lin_fs", big, 0 },
				{ "6e ASTC 1125x2436 pixel coordinates, clamp to zero", "tex_px_fs", big, 0 },
				{ "6f ASTC 1125x2436 at texture 3", "tex3_fs", big, 3 },
			};
			for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
				MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = [lib newFunctionWithName:@"quad_vs"];
				rpd.fragmentFunction = [lib newFunctionWithName:@(cases[c].fs)];
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
				err = nil;
				id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
				if (check("part 6: render pipeline", rps, err) || check("part 6: target", target, nil) ||
				    check("part 6: texture", cases[c].tex, nil)) {
					failures++;
					continue;
				}
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = target;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
				[enc setRenderPipelineState:rps];
				[enc setFragmentTexture:cases[c].tex atIndex:cases[c].index];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				    sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256
				    destinationBytesPerImage:64 * 256];
				[blit endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				const uint32_t *got = (const uint32_t *)out.contents;
				unsigned redPixels = 0;
				for (unsigned i = 0; i < 64 * 64; i++) {
					redPixels += got[i] == 0xff0000ffu;
				}
				say("MTLTEST: part %s: status %ld; %u of 4096 red; corners %08x %08x %08x %08x%s", cases[c].name, (long)cb.status,
				    redPixels, got[0], got[63], got[64 * 63], got[64 * 64 - 1], redPixels == 4096 ? "" : " FAIL");
				failures += redPixels != 4096 || cb.status != MTLCommandBufferStatusCompleted;
			}
		}
		sweep(dev, queue, lib);
		fetchTests(dev, queue, lib);
}

int
main(void)
{
	@autoreleasepool {
		con = open("/dev/console", O_WRONLY);
		if (con >= 0) {
			dup2(con, 1);
			dup2(con, 2);
		}
		pickTuDebug();
		say("MTLTEST: start (%s) TU_DEBUG=%s", getprogname(), getenv("TU_DEBUG") ? getenv("TU_DEBUG") : "-");
		run();
		say("MTLTEST: %s (%u failures)", failures ? "FAIL" : "PASS", failures);
	}
	return failures != 0;
}
