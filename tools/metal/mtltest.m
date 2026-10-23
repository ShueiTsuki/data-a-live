/*
 * mtltest: iOS Metal on the Adreno 840 through UniXNU's NativeMetal plugin (adapted by
 * adapt_ios16.py), the way an app reaches it: MTLCreateSystemDefaultDevice() runs Metal's own
 * discovery (IOAcceleratorES -> MetalPluginName -> NativeAdrenoMetal.bundle), and the plugin hands
 * a device only to processes named in NativeAdrenoMetal.clients. Three parts, logged on their own:
 *   1. the device (name, family support)
 *   2. compute: mtltest.metal's fill kernel over 4096 threads into a shared buffer, read back
 *   3. render: a triangle into a 64x64 RGBA8 texture, blitted to a buffer and checked
 * Lines go to /dev/console with the prefix "MTLTEST:" (the kernel log); stdout and stderr too.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include "mtltest_metallib.h"   /* mtltest.metallib as bytes (xxd -i), made by build_plugin.sh */

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
		dispatch_data_t data = dispatch_data_create(mtltest_metallib, mtltest_metallib_len, NULL,
		    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		id<MTLLibrary> lib = [dev newLibraryWithData:data error:&err];
		if (check("part 1: newLibraryWithData", lib, err)) {
			return;
		}
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
		say("MTLTEST: start (%s)", getprogname());
		run();
		say("MTLTEST: %s (%u failures)", failures ? "FAIL" : "PASS", failures);
	}
	return failures != 0;
}
