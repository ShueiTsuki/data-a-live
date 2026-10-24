#!/usr/bin/env python3
"""Adapt UniXNU NativeMetal (pinned c6d29ca, written for iPadOS 17 on an Adreno 740) to iOS 16.7 on the
Adreno 840 (ios-pandora). Exact, asserted text edits in a NativeMetal checkout; run once on a fresh tree.

  adapt_ios16.py NATIVEMETAL_DIR

1. NativeAdrenoMetalPlugin.mm: without NativeAdrenoMetal.publish the plugin used to start its stage-1
   self-test (raw ABI and PM4 tests written for the A740) in every process that runs Metal discovery.
   Here it returns the client device to processes named in NativeAdrenoMetal.clients and nil to all
   others, backboardd included; nothing else runs.
2. reportDisplayState writes into backboardd at iPadOS 17.7.11 QuartzCore addresses: disabled.
3. NativeBridgeMetalFrontend.mm: _MTLDevice is 528 bytes on iOS 16.7 (536 on iPadOS 17.7.11, the only
   size accepted); the dynamic subclass adds no instance variables, so both are accepted.
4. NativeTurnipSmoke.h: Turnip next to the plugin binary, else /usr/lib/libvulkan_freedreno.dylib
   (ios-pandora installs it there for tools/vktest; no second 17 MB copy inside the bundle).
"""
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1])


def edit(name, old, new):
    p = ROOT / name
    s = p.read_text()
    assert s.count(old) == 1, (name, old[:70])
    p.write_text(s.replace(old, new))
    print('edited', name)


edit('NativeAdrenoMetalPlugin.mm',
     """    if (publish::requested()) return publish::deviceForPort(port);
    static bool attempted;""",
     """    if (publish::requested()) return publish::deviceForPort(port);
    // ios-pandora (iOS 16, Adreno 840): no stage-1 self-test, it drives the A740's raw ABI. A process named in
    // NativeAdrenoMetal.clients gets its own device, every other process (backboardd too) nil and its software renderer.
    setenv("TU_DEBUG", "sysmem,nocb", 0);
    if (port) return publish::clientDevice(port);
    static bool attempted;""")
edit('NativeAdrenoMetalPlugin.mm',
     """static void reportDisplayState(Gpu &gpu) {
    ++gReports;""",
     """static void reportDisplayState(Gpu &gpu) {
    if (gReports >= 0) return;   // ios-pandora: the addresses below are iPadOS 17.7.11 QuartzCore's, not iOS 16's
    ++gReports;""")
edit('NativeAdrenoMetalPlugin.mm',
     """    if (!device) clientFailure(port, usable ? "no-device" : "no-cachedir", lastError);
    return device;""",
     """    if (!device) clientFailure(port, usable ? "no-device" : "no-cachedir", lastError);
    else if (!strcmp(getprogname(), "backboardd")) {
        // ios-pandora: the render server as a client (no publish mode here): what it reads for the deep
        // framebuffer, and the bridge's telemetry (refused surfaces, last problems) every 30 s.
        CFTypeRef deep = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication);
        NSLog(@"pandora-metal: backboardd has a device; CAEnableDeepFramebuffer %@", (__bridge id)deep);
        if (deep) CFRelease(deep);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, [](void *) -> void * {
                static char table[6144];
                for (unsigned round = 0; round < 10; ++round) {
                    sleep(30);
                    native_bridge_metal_telemetry(table, sizeof table);
                    for (char *line = table, *end; *line; line = end) {
                        end = strchr(line, '\\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal T%u %s", round, line);
                    }
                }
                return nullptr;
            }, nullptr) == 0) pthread_detach(thread);
    }
    return device;""")
edit('NativeBridgeMetalFrontend.mm',
     """static NSError *nbError(NSString *why) {
 return""",
     """static NSError *nbError(NSString *why) {
 NSLog(@"pandora-metal refused: %@", why);   // ios-pandora: every refusal in the log (backboardd: specialize, bb7)
 return""")
edit('NativeBridgeMetalFrontend.mm',
     """  case 84:case 88:return 32;
  default:return 0;""",
     """  case 84:case 88:return 32;
  // ios-pandora: 3-wide vectors, lanes packed as metal2vulkan's canon/fc.rs reads them (the first
  // lanes*width bytes; Metal's storage is at least that). iOS 16 backboardd specializes a uchar3 (bb8).
  case 5:case 31:case 35:return 12;
  case 18:case 39:case 43:return 6;
  case 47:case 51:case 55:return 3;
  case 83:case 87:return 24;
  default:return 0;""")
edit('NativeBridgeMetalFrontend.mm',
     """ (void)bytes;(void)length;(void)d;(void)deallocator;
 NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of raw memory is not known)");
 return nil;""",
     """ // ios-pandora: a one-level 2D texture in rows of texels or blocks (bb25: QuartzCore in backboardd asks for the App
 // Library's full-screen ASTC 4x4 image, shader-read only, 282 x 609 blocks of 16 bytes). Made as an ordinary texture
 // and filled from the bytes once; the caller's deallocator runs when the texture goes. A read-only texture sees no
 // later writes to the bytes, which a texture over them would.
 static const uint8_t astc[16][2]={{4,2},{4,4},{5,4},{5,5},{6,5},{6,6},{8,4},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 const NSUInteger format=d.pixelFormat;
 NSUInteger bw=1,bh=1,bb=0;
 if(format>=185 && format<=200){bw=astc[format-185][0];bh=astc[format-185][1];bb=16;}
 else if(format>=203 && format<=218){bw=astc[format-203][0];bh=astc[format-203][1];bb=16;}
 else switch(format){case 1:case 10:bb=1;break;case 30:bb=2;break;case 70:case 71:case 80:case 81:bb=4;break;case 115:bb=8;break;default:break;}
 const NSUInteger rowBytes=(d.width+bw-1)/bw*bb,rows=(d.height+bh-1)/bh;
 if(bytes && bb && d.textureType==MTLTextureType2D && d.mipmapLevelCount==1 && d.arrayLength==1 && d.depth==1 &&
    d.sampleCount==1 && length>=rowBytes*rows) {
  id<MTLTexture> texture=[(id<MTLDevice>)self newTextureWithDescriptor:d];
  if(texture) {
   [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
   if(deallocator) {
    static char ownerKey;
    NSData *owner=[[NSData alloc] initWithBytesNoCopy:bytes length:length deallocator:^(void *b,NSUInteger l){deallocator(b,l);}];
    objc_setAssociatedObject(texture,&ownerKey,owner,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   }
   static std::atomic<uint32_t> made{0};
   if(++made<=5)NSLog(@"pandora-metal nocopy: made fmt %lu %lux%lu, %lu bytes per row, copied once",(unsigned long)format,
                      (unsigned long)d.width,(unsigned long)d.height,(unsigned long)rowBytes);
   return texture;
  }
 }
 NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of raw memory is not known)");
 // ios-pandora: what is asked for and still refused: the descriptor, the memory and the callers (image + offset), the
 // first 20 calls and then one in 100.
 static std::atomic<uint32_t> asked{0};
 const uint32_t nth=++asked;
 if(nth<=20 || nth%100==0) {
  void *frames[10];const int n=backtrace(frames,10);
  std::string callers;char item[96];
  for(int i=1;i<n;i++) {
   Dl_info info{};
   if(!dladdr(frames[i],&info) || !info.dli_fname)continue;
   const char *slash=strrchr(info.dli_fname,'/');
   snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));
   callers+=item;
  }
  NSLog(@"pandora-metal nocopy #%u: fmt %lu type %lu %lux%lux%lu mips %lu array %lu usage %lx storage %lu opts %lx"
        " bytes %p (page +%lx) length %lu (per row if one plane: %lu) |%s",nth,(unsigned long)d.pixelFormat,
        (unsigned long)d.textureType,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.depth,
        (unsigned long)d.mipmapLevelCount,(unsigned long)d.arrayLength,(unsigned long)d.usage,(unsigned long)d.storageMode,
        (unsigned long)d.resourceOptions,bytes,(unsigned long)((uintptr_t)bytes&0x3fff),(unsigned long)length,
        (unsigned long)(d.height?length/d.height:0),callers.c_str());
 }
 return nil;""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(!base || class_getInstanceSize(base)!=536){""",
     """ // ios-pandora: 528 bytes on iOS 16.7 (ios-fuxi docs-lanes/metal.md), 536 on iPadOS 17.7.11.
 if(!base || (class_getInstanceSize(base)!=536 && class_getInstanceSize(base)!=528)){""")
edit('NativeTurnipSmoke.h',
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    return true;""",
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    if (access(path, R_OK) != 0) snprintf(path, size, "%s", "/usr/lib/libvulkan_freedreno.dylib");   // ios-pandora
    return true;""")

# ---- ios-pandora: frame capture for the App Library (bb26) -------------------------------------------------
# Its full-screen blur draws empty (the zoomed home screen snapshot behind shows through, the rest is black),
# with no refusal on record. Right after newTextureWithBytesNoCopy makes the App Library's image, the next
# 800 events backboardd encodes are logged as "pandora-metal cap": render passes (attachments, level, load and
# store actions, clear colour), pipelines (both functions, colour format), textures bound (size, format, mip
# levels, type, view parent, IOSurface), viewports, scissors, draws, texture copies, mipmap generation and
# commits. Twice per boot at most.
edit('NativeBridgeMetalFrontend.mm',
     """#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];NSLog(@"%@",nbText);noteProblem(nbText);}while(0)
""",
     """#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];NSLog(@"%@",nbText);noteProblem(nbText);}while(0)
// ios-pandora: the App Library capture (tools/metal/adapt_ios16.py).
static std::atomic<int> gCapture{0};
static char gCapNamesKey;
static inline bool capturing() {return gCapture.load(std::memory_order_relaxed)>0;}
static void capture(NSString *line) {if(gCapture.fetch_sub(1,std::memory_order_relaxed)>0)NSLog(@"pandora-metal cap %@",line);}
static NSString *capId(id object) {return [NSString stringWithFormat:@"%05lx",(unsigned long)(((uintptr_t)(__bridge void *)object>>4)&0xfffff)];}
static NSString *capTexture(id<MTLTexture> t) {
 if(!t)return @"-";
 id<MTLTexture> parent=[(id)t respondsToSelector:@selector(parentTexture)]?t.parentTexture:nil;
 const bool surface=[(id)t respondsToSelector:@selector(iosurface)] && t.iosurface;
 return [NSString stringWithFormat:@"%@ %lux%lu f%lu m%lu t%lu%@%@%@",capId(t),(unsigned long)t.width,(unsigned long)t.height,
  (unsigned long)t.pixelFormat,(unsigned long)t.mipmapLevelCount,(unsigned long)t.textureType,surface?@" surface":@"",
  parent?@" view-of ":@"",parent?capId(parent):@""];
}
""")
edit('NativeBridgeMetalFrontend.mm',
     """   if(++made<=5)NSLog(@"pandora-metal nocopy: made fmt""",
     """   static std::atomic<int> captures{0};
   if(captures.fetch_add(1)<2)gCapture.store(800);
   if(++made<=5)NSLog(@"pandora-metal nocopy: made fmt""")
edit('NativeBridgeMetalFrontend.mm',
     """ NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLRenderPipelineState>)state;""",
     """ NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;
 objc_setAssociatedObject(state,&gCapNamesKey,[NSString stringWithFormat:@"%@ / %@ c0 f%lu blend %d",d.vertexFunction.name,d.fragmentFunction.name,
  (unsigned long)d.colorAttachments[0].pixelFormat,(int)d.colorAttachments[0].blendingEnabled],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return (id<MTLRenderPipelineState>)state;""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
""",
     """- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(capturing())capture([NSString stringWithFormat:@"pipe %@ %@",capId(state),objc_getAssociatedObject(state,&gCapNamesKey)]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
""",
     """- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(capturing())capture([NSString stringWithFormat:@"tex %s%lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,capTexture(texture)]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)setViewport:(MTLViewport)v {
""",
     """- (void)setViewport:(MTLViewport)v {
 if(capturing())capture([NSString stringWithFormat:@"viewport %.1f %.1f %.1fx%.1f",v.originX,v.originY,v.width,v.height]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)setScissorRect:(MTLScissorRect)r {
""",
     """- (void)setScissorRect:(MTLScissorRect)r {
 if(capturing())capture([NSString stringWithFormat:@"scissor %lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
""",
     """- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 if(capturing())capture([NSString stringWithFormat:@"draw p%lu v%lu+%lu i%lu",(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
""",
     """                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(capturing())capture([NSString stringWithFormat:@"drawidx p%lu n%lu i%lu",(unsigned long)t,(unsigned long)n,(unsigned long)i]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
""",
     """- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 if(capturing()) {
  NSMutableString *line=[NSMutableString stringWithFormat:@"pass rt %lux%lu",(unsigned long)d.renderTargetWidth,(unsigned long)d.renderTargetHeight];
  for(unsigned i=0;i<4;i++) {
   MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];if(!c.texture)continue;
   [line appendFormat:@" | c%u %@ lv%lu sl%lu load%lu store%lu clear %.2f,%.2f,%.2f,%.2f%@",i,capTexture(c.texture),(unsigned long)c.level,
    (unsigned long)c.slice,(unsigned long)c.loadAction,(unsigned long)c.storeAction,c.clearColor.red,c.clearColor.green,c.clearColor.blue,
    c.clearColor.alpha,c.resolveTexture?[@" resolve " stringByAppendingString:capTexture(c.resolveTexture)]:@""];
  }
  if(d.depthAttachment.texture)[line appendFormat:@" | depth %@ load%lu",capTexture(d.depthAttachment.texture),(unsigned long)d.depthAttachment.loadAction];
  if(d.stencilAttachment.texture)[line appendFormat:@" | stencil %@ load%lu",capTexture(d.stencilAttachment.texture),(unsigned long)d.stencilAttachment.loadAction];
  capture(line);
 }
""")
edit('NativeBridgeMetalFrontend.mm',
     """              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
""",
     """              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if(capturing())capture([NSString stringWithFormat:@"copy %@ lv%lu %lu,%lu %lux%lu -> %@ lv%lu %lu,%lu",capTexture(s),(unsigned long)sl,(unsigned long)so.x,
  (unsigned long)so.y,(unsigned long)z.width,(unsigned long)z.height,capTexture(d),(unsigned long)dl,(unsigned long)dO.x,(unsigned long)dO.y]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
""",
     """- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(capturing())capture([NSString stringWithFormat:@"mipmaps %@",capTexture(t)]);
""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 self.committed=YES;
""",
     """ if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 if(capturing())capture(@"commit");
 self.committed=YES;
""")
