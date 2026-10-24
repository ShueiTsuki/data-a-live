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
