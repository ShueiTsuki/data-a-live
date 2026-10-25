#!/usr/bin/env python3
"""Adapt UniXNU NativeMetal (pinned c6d29ca, written for iPadOS 17 on an Adreno 740) to iOS 16.7 on the
Adreno 840 (data-a-live). Exact, asserted text edits in a NativeMetal checkout; run once on a fresh tree.

  adapt_ios16.py NATIVEMETAL_DIR

1. NativeAdrenoMetalPlugin.mm: without NativeAdrenoMetal.publish the plugin used to start its stage-1
   self-test (raw ABI and PM4 tests written for the A740) in every process that runs Metal discovery.
   Here it returns the client device to processes named in NativeAdrenoMetal.clients and nil to all
   others, backboardd included; nothing else runs.
2. reportDisplayState writes into backboardd at iPadOS 17.7.11 QuartzCore addresses: disabled.
3. NativeBridgeMetalFrontend.mm: _MTLDevice is 528 bytes on iOS 16.7 (536 on iPadOS 17.7.11, the only
   size accepted); the dynamic subclass adds no instance variables, so both are accepted.
4. NativeTurnipSmoke.h: Turnip next to the plugin binary, else /usr/lib/libvulkan_freedreno.dylib
   (data-a-live installs it there for tools/vktest; no second 17 MB copy inside the bundle).
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
     """#include <pthread.h>
""",
     """#include <pthread.h>
#include <sys/sysctl.h>
""")
edit('NativeAdrenoMetalPlugin.mm',
     """    if (publish::requested()) return publish::deviceForPort(port);
    static bool attempted;""",
     """    if (publish::requested()) return publish::deviceForPort(port);
    // data-a-live (iOS 16, Adreno 840): no stage-1 self-test, it drives the A740's raw ABI. A process named in
    // NativeAdrenoMetal.clients gets its own device, every other process (backboardd too) nil and its software renderer.
    {   // data-a-live: TU_DEBUG from the boot-arg pandora_tu_debug=a,b,c when given (bb37), else nocb; a value
        // already in the environment wins either way (mtltest picks its own per launch). Without sysmem Turnip
        // chooses per render pass between on-chip tiles (GMEM) and memory (its autotune), its default on other
        // Adrenos; nocb keeps concurrent binning off (gen8 BV/BR stalls seen elsewhere, not yet understood here).
        static char chosen[128];
        const char *flags = "nocb";
        char args[1024] = {};
        size_t size = sizeof args - 1;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0)
            if (const char *at = strstr(args, "pandora_tu_debug=")) {
                snprintf(chosen, sizeof chosen, "%.*s", (int)strcspn(at + 17, " "), at + 17);
                flags = chosen;
            }
        setenv("TU_DEBUG", flags, 0);
        static bool told;
        if (!told) {
            told = true;
            NSLog(@"pandora-metal TU_DEBUG=%s", getenv("TU_DEBUG"));
        }
    }
    if (port) return publish::clientDevice(port);
    static bool attempted;""")
edit('NativeAdrenoMetalPlugin.mm',
     """static void reportDisplayState(Gpu &gpu) {
    ++gReports;""",
     """static void reportDisplayState(Gpu &gpu) {
    if (gReports >= 0) return;   // data-a-live: the addresses below are iPadOS 17.7.11 QuartzCore's, not iOS 16's
    ++gReports;""")
edit('NativeAdrenoMetalPlugin.mm',
     """    if (!device) clientFailure(port, usable ? "no-device" : "no-cachedir", lastError);
    return device;""",
     """    if (!device) clientFailure(port, usable ? "no-device" : "no-cachedir", lastError);
    else if (!strcmp(getprogname(), "backboardd")) {
        // data-a-live: the render server as a client (no publish mode here): what it reads for the deep
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
 NSLog(@"pandora-metal refused: %@", why);   // data-a-live: every refusal in the log (backboardd: specialize, bb7)
 return""")
edit('NativeBridgeMetalFrontend.mm',
     """  case 84:case 88:return 32;
  default:return 0;""",
     """  case 84:case 88:return 32;
  // data-a-live: 3-wide vectors, lanes packed as metal2vulkan's canon/fc.rs reads them (the first
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
     """ // data-a-live: a one-level 2D texture in rows of texels or blocks (bb25: QuartzCore in backboardd asks for the App
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
 // data-a-live: what is asked for and still refused: the descriptor, the memory and the callers (image + offset), the
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
     """ // data-a-live: 528 bytes on iOS 16.7 (ios-fuxi docs-lanes/metal.md), 536 on iPadOS 17.7.11.
 if(!base || (class_getInstanceSize(base)!=536 && class_getInstanceSize(base)!=528)){""")
edit('NativeTurnipSmoke.h',
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    return true;""",
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    if (access(path, R_OK) != 0) snprintf(path, size, "%s", "/usr/lib/libvulkan_freedreno.dylib");   // data-a-live
    return true;""")

# ---- data-a-live: frame capture for the App Library (bb26) -------------------------------------------------
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
// data-a-live: the App Library capture (tools/metal/adapt_ios16.py).
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

# ---- data-a-live: what the App Library's ASTC bytes look like (bb28) ------------------------------------------
# Re-uploading at commit (38391ce) never fired for the App Library (its bytes do not change after the texture is
# made) and froze backboardd in the app switcher, so it is gone. The image still shows a sharp picture in one part
# and black elsewhere. For a block-compressed texture the next lines describe the bytes as rows of blocks: an 8x8
# grid of the image, each cell the share of all-zero blocks (z), ASTC void-extent (single colour) blocks (v) and
# other blocks (o), so the picture's layout can be compared with the screen, and the callers.
edit('NativeBridgeMetalFrontend.mm',
     """   static std::atomic<int> captures{0};""",
     """   if(bb==16) {   // data-a-live: the block layout of the bytes, 8x8 grid
    static std::atomic<int> described{0};
    if(described.fetch_add(1)<6) {
     const uint8_t *blocks=(const uint8_t *)bytes;
     unsigned zero[8][8]={},single[8][8]={},other[8][8]={};
     for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<rowBytes/16;x++) {
      const uint8_t *b=blocks+y*rowBytes+x*16;
      bool z=true;for(unsigned k=0;k<16;k++)if(b[k]){z=false;break;}
      const unsigned gy=(unsigned)(y*8/rows),gx=(unsigned)(x*8/(rowBytes/16));
      if(z)zero[gy][gx]++;else if(((b[0]|(b[1]<<8))&0x1ff)==0x1fc)single[gy][gx]++;else other[gy][gx]++;
     }
     void *frames[8];const int n=backtrace(frames,8);std::string callers;char item[96];
     for(int i=1;i<n;i++){Dl_info info{};if(!dladdr(frames[i],&info) || !info.dli_fname)continue;const char *slash=strrchr(info.dli_fname,'/');
      snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));callers+=item;}
     const unsigned image=(unsigned)described.load();
     NSLog(@"pandora-metal nocopy blocks #%u %lux%lu %lu per row |%s",image,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)rowBytes,callers.c_str());
     for(unsigned gy=0;gy<8;gy++) {
      NSMutableString *line=[NSMutableString string];
      for(unsigned gx=0;gx<8;gx++) {
       const unsigned all=zero[gy][gx]+single[gy][gx]+other[gy][gx];
       [line appendFormat:@" %02u/%02u/%02u",all?zero[gy][gx]*99/all:0,all?single[gy][gx]*99/all:0,all?other[gy][gx]*99/all:0];
      }
      NSLog(@"pandora-metal nocopy grid #%u r%u z/v/o:%@",image,gy,line);
     }
    }
   }
   static std::atomic<int> captures{0};""")

# ---- data-a-live: no-copy memory is cleaned like surfaces (bb30) ---------------------------------------------
# The kernel maps imported user memory for the GPU uncached and leaves the CPU's caches to the importer
# (NativeAdrenoABI.h; pandora_gpu.c flushes every import only with pandora_gpu_flushall=1). NativeMetal cleans
# the IOSurfaces a command buffer uses, but not memory from newBufferWithBytesNoCopy or textures over it, so
# the GPU can read what the CPU has not written back yet (bb30: the Settings icon showed only its top rows).
# Now those ranges are cleaned and invalidated at commit and at completion too, as the surfaces are.
edit('NativeBridgeMetalFrontend.mm',
     """static NBTexture *textureOnBuffer(NBBuffer *buffer,""",
     """// data-a-live: no-copy buffers a command uses, directly or under a texture (tools/metal/adapt_ios16.py).
static std::atomic<uint64_t> gWrappedFlushes{0},gWrappedFlushBytes{0};
static void flushWrapped(NSSet *owners) {
 if(!owners.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 for(id o in owners) {
  NBBuffer *b=nil;
  if([o isKindOfClass:[NBBuffer class]])b=o;
  else if([o isKindOfClass:[NBTexture class]])b=((NBTexture *)o).buffer;
  if(b && b.wrapped)ranges.emplace_back(reinterpret_cast<uintptr_t>(b.wrapped),(size_t)[(id<MTLBuffer>)b length]);
 }
 if(ranges.empty())return;
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 for(auto it=ranges.begin();it!=last;++it){sys_dcache_flush(reinterpret_cast<void *>(it->first),it->second);gWrappedFlushBytes+=it->second;}
 const uint64_t n=++gWrappedFlushes;
 if(n<=3 || n%2000==0)NSLog(@"pandora-metal wrapped: %llu cleanings, %llu MiB in all",(unsigned long long)n,(unsigned long long)(gWrappedFlushBytes.load()>>20));
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,""")
edit('NativeBridgeMetalFrontend.mm',
     """ flushSurfaces(_surfaces);
 // Actual GPU completion""",
     """ flushSurfaces(_surfaces);
 flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
 // Actual GPU completion""")
edit('NativeBridgeMetalFrontend.mm',
     """  flushSurfaces(_surfaces);
  // Completion owns this reference""",
     """  flushSurfaces(_surfaces);
  flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
  // Completion owns this reference""")

# ---- data-a-live: refused IOSurface textures in the log (bb30) -----------------------------------------------
# A texture over an IOSurface that the frontend cannot make is only counted (bb30: 148 of 173 refused, the last
# a 1125x2436 two-plane RGB10A8_2P_XR10 surface). Its layer is then not drawn, which could be the App Library's
# missing background. The first refusals now say what they were and who asked.
edit('NativeBridgeMetalFrontend.mm',
     """ std::lock_guard<std::mutex> lock(gProblemLock);gSurfaceLastRefusal=r;
}""",
     """ std::lock_guard<std::mutex> lock(gProblemLock);gSurfaceLastRefusal=r;
 static std::atomic<int> told{0};
 if(told.fetch_add(1)<40) {
  void *frames[8];const int n=backtrace(frames,8);std::string callers;char item[96];
  for(int i=1;i<n;i++){Dl_info info{};if(!dladdr(frames[i],&info) || !info.dli_fname)continue;const char *slash=strrchr(info.dli_fname,'/');
   snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));callers+=item;}
  const uint32_t f=r.format;
  NSLog(@"pandora-metal surface refused: step %u '%c%c%c%c' planes %u plane %u pf %u %llux%llu row %llu offset %llu alloc %llu |%s",
        r.step,(char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,r.planes,r.plane,r.pixelFormat,(unsigned long long)r.width,
        (unsigned long long)r.height,(unsigned long long)r.row,(unsigned long long)r.offset,(unsigned long long)r.allocation,callers.c_str());
 }
}""")

# ---- data-a-live: App Library probe, boot-arg pandora_astc_probe=1 (bb31) -----------------------------------
# While a finger holds the App Library its background is right (QuartzCore blurs live); at rest it is drawn from
# the two pre-blurred ASTC images and comes out transparent. With the boot-arg the left half of each block row of a
# large ASTC image is replaced by opaque red single-colour (void-extent) blocks before the upload: red on the left
# and the picture on the right = all fine; red and transparent = the real blocks are decoded wrong; nothing = the
# drawing or compositing after the upload is at fault.
edit('NativeBridgeMetalFrontend.mm',
     """#include <execinfo.h>
""",
     """#include <execinfo.h>
#include <sys/sysctl.h>
""")
edit('NativeBridgeMetalFrontend.mm',
     """   [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
   if(deallocator) {""",
     """   const int probeLevel=pandoraAstcProbeLevel();const bool probe=probeLevel>=1;
   if(probe && bb==16 && d.width>=512) {
    std::vector<uint8_t> copy((const uint8_t *)bytes,(const uint8_t *)bytes+rowBytes*rows);
    static const uint8_t red[16]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff};
    const NSUInteger blocks=rowBytes/16;
    for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<blocks/2;x++)memcpy(&copy[y*rowBytes+x*16],red,16);
    static std::atomic<int> handed{0};
    const int handedNth=handed.fetch_add(1);
    // Level 2: every large ASTC image is handed over as BGRA8; level 3: every other one (the first BGRA8, the second
    // ASTC with its left half red), so one run shows both.
    if(probeLevel==2 || (probeLevel>=3 && handedNth%2==0)) {
     // bb35: the ASTC blocks are in the texture exactly as uploaded, yet no red is drawn. Level 2 hands QuartzCore a
     // BGRA8 texture of the same size instead (left half red, right half green): colour on screen = only ASTC
     // sampling is wrong; still transparent = the sampling or the draw is wrong whatever the format.
     MTLTextureDescriptor *plain=[d copy];plain.pixelFormat=MTLPixelFormatBGRA8Unorm;
     id<MTLTexture> swap=[(id<MTLDevice>)self newTextureWithDescriptor:plain];
     if(swap) {
      std::vector<uint32_t> px((size_t)d.width*d.height);
      for(NSUInteger y=0;y<d.height;y++)for(NSUInteger x=0;x<d.width;x++)px[y*d.width+x]=x<d.width/2?0xffff0000u:0xff00ff00u;
      [swap replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:px.data() bytesPerRow:d.width*4];
      texture=swap;
      objc_setAssociatedObject(swap,&gProbeReadbackKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);   // read back its targets too
      NSLog(@"pandora-metal astc probe: %lux%lu handed over as BGRA8, left red right green",(unsigned long)d.width,(unsigned long)d.height);
     }
    } else {
    // bb34: no red reached any render target. The first image is uploaded by replaceRegion, the second through a
    // shared buffer and a blit; each is then copied back by the GPU and compared block by block with what went up.
    static std::atomic<int> nth{0};
    const int which=nth.fetch_add(1);
    static id<MTLCommandQueue> queue=[(id<MTLDevice>)self newCommandQueue];
    const NSUInteger size=rowBytes*rows;
    if(which%2==0)
     [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:copy.data() bytesPerRow:rowBytes];
    else {
     id<MTLBuffer> up=[(id<MTLDevice>)self newBufferWithBytes:copy.data() length:size options:MTLResourceStorageModeShared];
     id<MTLCommandBuffer> c=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[c blitCommandEncoder];
     [b copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:size sourceSize:MTLSizeMake(d.width,d.height,1)
             toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
     [b endEncoding];[c commit];[c waitUntilCompleted];
    }
    id<MTLBuffer> back=[(id<MTLDevice>)self newBufferWithLength:size options:MTLResourceStorageModeShared];
    memset(back.contents,0xa5,size);
    id<MTLCommandBuffer> c=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[c blitCommandEncoder];
    [b copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(d.width,d.height,1)
              toBuffer:back destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:size];
    [b endEncoding];[c commit];[c waitUntilCompleted];
    unsigned same[2]={},zero[2]={},untouched[2]={},other[2]={};
    const uint8_t *got=(const uint8_t *)back.contents;
    for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<blocks;x++) {
     const uint8_t *g=got+y*rowBytes+x*16,*w=&copy[y*rowBytes+x*16];const int half=x<blocks/2?0:1;
     bool z=true,u=true;for(int k=0;k<16;k++){if(g[k])z=false;if(g[k]!=0xa5)u=false;}
     if(!memcmp(g,w,16))same[half]++;else if(z)zero[half]++;else if(u)untouched[half]++;else other[half]++;
    }
    NSLog(@"pandora-metal astc probe #%d %lux%lu by %s, left %lu of %lu block columns red; read back left same/zero/untouched/other "
          "%u/%u/%u/%u right %u/%u/%u/%u (status %ld)",which,(unsigned long)d.width,(unsigned long)d.height,which%2==0?"replaceRegion":"blit",
          (unsigned long)(blocks/2),(unsigned long)blocks,same[0],zero[0],untouched[0],other[0],same[1],zero[1],untouched[1],other[1],(long)c.status);
    }
   } else
   [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
   if(deallocator) {""")

# ---- data-a-live: App Library readback, same boot-arg (bb32) --------------------------------------------------
# bb32: with the left halves of both ASTC images red, no red reached the screen at all; the user saw the App
# Library as always. So after the upload something is lost. With pandora_astc_probe=1 the render target a large
# ASTC texture is drawn into is now read back when that command buffer completes (off the completion thread) and
# logged as an 8x8 grid of average colours (RGBA hex): red there = the draw works and the compositing loses it.
edit('NativeBridgeMetalFrontend.mm',
     """#include <sys/sysctl.h>
""",
     """#include <sys/sysctl.h>
static int pandoraAstcProbeLevel() {   // data-a-live: boot-arg pandora_astc_probe=N (tools/metal/adapt_ios16.py)
 static const int level=[]{char args[1024]={};size_t size=sizeof args-1;
  if(sysctlbyname("kern.bootargs",args,&size,nullptr,0)!=0)return 0;
  const char *at=strstr(args,"pandora_astc_probe=");return at?atoi(at+19):0;}();
 return level;
}
static bool pandoraAstcProbe() {return pandoraAstcProbeLevel()>=1;}
static char gProbePassTargetKey,gProbeReadbackKey,gProbeSamplerKey,gProbeBindingsKey,gProbePipeKey;
// data-a-live: an encoder's bindings as the probe's draw log prints them (tools/metal/adapt_ios16.py).
static NSMutableDictionary *probeBindings(id encoder) {
 NSMutableDictionary *seen=objc_getAssociatedObject(encoder,&gProbeBindingsKey);
 if(!seen){seen=[NSMutableDictionary dictionary];objc_setAssociatedObject(encoder,&gProbeBindingsKey,seen,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 return seen;
}
static NSString *probeHex(const void *bytes,NSUInteger length,NSUInteger most) {
 NSMutableString *hex=[NSMutableString stringWithFormat:@"%lu:",(unsigned long)length];
 for(NSUInteger k=0;bytes && k<length && k<most;k++)[hex appendFormat:@"%s%02x",k%4?"":" ",((const uint8_t *)bytes)[k]];
 return hex;
}
static const char *probeStage(uint32_t stage) {return stage==1?"v":stage==2?"f":"c";}
// One binding of the probe's draw log, in pieces of 160 characters (bb36: the kernel log split longer lines and lost
// characters at each split): "replay N: key [i/n] piece".
static void probeLog(NSString *draw,NSString *key,id value) {
 NSString *text=[value description];
 const NSUInteger piece=160,count=MAX((NSUInteger)1,(text.length+piece-1)/piece);
 for(NSUInteger i=0;i<count;i++)
  NSLog(@"pandora-metal replay %@: %@ [%lu/%lu] %@",draw,key,(unsigned long)(i+1),(unsigned long)count,
        [text substringWithRange:NSMakeRange(i*piece,MIN(piece,text.length-i*piece))]);
}
""")
edit('NativeBridgeMetalFrontend.mm',
     """ return [self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];""",
     """ id<MTLRenderCommandEncoder> made=[self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
 if(made && pandoraAstcProbe() && d.colorAttachments[0].texture)   // data-a-live: the pass's target, for the readback
  objc_setAssociatedObject(made,&gProbePassTargetKey,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return made;""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 [self check:nmb_encoder_set_texture""",
     """ if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 if(texture && pandoraAstcProbe() && ((texture.pixelFormat==MTLPixelFormatASTC_4x4_LDR && texture.width>=512) ||
                                       objc_getAssociatedObject(texture,&gProbeReadbackKey))) {
  id target=objc_getAssociatedObject(self,&gProbePassTargetKey);   // data-a-live: read it back at completion
  if(target) {
   NSMutableSet *targets=objc_getAssociatedObject(_commandBuffer,&gProbeReadbackKey);
   if(!targets){targets=[NSMutableSet set];objc_setAssociatedObject(_commandBuffer,&gProbeReadbackKey,targets,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   [targets addObject:target];
  }
 }
 [self check:nmb_encoder_set_texture""")
edit('NativeBridgeMetalFrontend.mm',
     """ @synchronized(gInFlight){[gInFlight removeObject:self];}
 NSArray *completedHandlers;""",
     """ @synchronized(gInFlight){[gInFlight removeObject:self];}
 if(NSSet *targets=objc_getAssociatedObject(self,&gProbeReadbackKey)) {   // data-a-live: the App Library readback
  static std::atomic<int> readbacks{0};
  id<MTLCommandQueue> queue=(id<MTLCommandQueue>)self.queue;
  for(id<MTLTexture> target in [targets copy]) {
   if(readbacks.fetch_add(1)>=12)break;
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
    const NSUInteger w=target.width,h=target.height,format=target.pixelFormat;
    if(format!=MTLPixelFormatBGRA8Unorm && format!=MTLPixelFormatBGRA8Unorm_sRGB && format!=MTLPixelFormatRGBA8Unorm) {
     NSLog(@"pandora-metal astc readback: target %lux%lu format %lu, not read",(unsigned long)w,(unsigned long)h,(unsigned long)format);return;
    }
    std::vector<uint8_t> pixels(w*h*4,0xa5);
    if(target.storageMode==MTLStorageModePrivate) {
     // Private: the GPU copies it into a shared buffer first (bb33: getBytes refuses private textures).
     id<MTLBuffer> staging=[target.device newBufferWithLength:w*h*4 options:MTLResourceStorageModeShared];
     id<MTLCommandBuffer> copy=[queue commandBuffer];
     id<MTLBlitCommandEncoder> blit=[copy blitCommandEncoder];
     [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
                  toBuffer:staging destinationOffset:0 destinationBytesPerRow:w*4 destinationBytesPerImage:w*h*4];
     [blit endEncoding];[copy commit];[copy waitUntilCompleted];
     if(staging.contents)memcpy(pixels.data(),staging.contents,w*h*4);
    } else
    [target getBytes:pixels.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    const bool bgra=format!=MTLPixelFormatRGBA8Unorm;
    NSLog(@"pandora-metal astc readback: target %lux%lu format %lu storage %lu%s",(unsigned long)w,(unsigned long)h,(unsigned long)format,
          (unsigned long)target.storageMode,[(NBTexture *)target surface]?" surface":"");
    for(unsigned gy=0;gy<8;gy++) {
     NSMutableString *line=[NSMutableString string];
     for(unsigned gx=0;gx<8;gx++) {
      uint64_t sum[4]={},n=0;
      for(NSUInteger y=gy*h/8;y<(gy+1)*h/8;y+=4)for(NSUInteger x=gx*w/8;x<(gx+1)*w/8;x+=4) {
       const uint8_t *p=&pixels[(y*w+x)*4];
       sum[0]+=p[bgra?2:0];sum[1]+=p[1];sum[2]+=p[bgra?0:2];sum[3]+=p[3];n++;
      }
      if(!n)n=1;
      [line appendFormat:@" %02x%02x%02x%02x",(unsigned)(sum[0]/n),(unsigned)(sum[1]/n),(unsigned)(sum[2]/n),(unsigned)(sum[3]/n)];
     }
     NSLog(@"pandora-metal astc readback r%u:%@",gy,line);
    }
   });
  }
 }
 NSArray *completedHandlers;""")

# ---- data-a-live: sampler states in the log with the probe (bb35) --------------------------------------------
edit('NativeBridgeMetalFrontend.mm',
     """ NmbSampler *handle=nmb_sampler_new(gDevice,&s);""",
     """ if(pandoraAstcProbe()) {   // data-a-live: what QuartzCore samples with (tools/metal/adapt_ios16.py)
  static std::atomic<int> told{0};
  if(told.fetch_add(1)<40)NSLog(@"pandora-metal sampler: min %lu mag %lu mip %lu address %lu/%lu/%lu border %lu normalized %d lod %.1f-%.1f compare %lu",
   (unsigned long)d.minFilter,(unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,
   (unsigned long)d.rAddressMode,(unsigned long)d.borderColor,(int)d.normalizedCoordinates,d.lodMinClamp,d.lodMaxClamp,(unsigned long)d.compareFunction);
 }
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);""")

# ---- data-a-live: what the App Library draw binds, probe level 3 (bb36) ---------------------------------------
# The samplers and fragment bytes bound when a probed texture is bound, for the first draws: with the samplers'
# descriptors (kept on each sampler as it is made) and the first 64 bytes of each fragment bytes binding.
edit('NativeBridgeMetalFrontend.mm',
     """ NBSampler *sampler=[NBSampler new];sampler.handle=handle;return (id<MTLSamplerState>)sampler;""",
     """ NBSampler *sampler=[NBSampler new];sampler.handle=handle;
 if(pandoraAstcProbe())objc_setAssociatedObject(sampler,&gProbeSamplerKey,[NSString stringWithFormat:@"min%lu mag%lu mip%lu addr%lu/%lu border%lu norm%d",
  (unsigned long)d.minFilter,(unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,
  (unsigned long)d.borderColor,(int)d.normalizedCoordinates],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return (id<MTLSamplerState>)sampler;""")
edit('NativeBridgeMetalFrontend.mm',
     """ [self check:nmb_encoder_set_sampler(_handle,stage,""",
     """ if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log
  NSString *what=sampler?objc_getAssociatedObject(sampler,&gProbeSamplerKey):nil;
  probeBindings(self)[[NSString stringWithFormat:@"%ssampler%lu",probeStage(stage),(unsigned long)index]]=what?:(sampler?@"?":@"none");
 }
 [self check:nmb_encoder_set_sampler(_handle,stage,""")
edit('NativeBridgeMetalFrontend.mm',
     """ [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];""",
     """ if(pandoraAstcProbe() && bytes)   // data-a-live: remembered for the probe's draw log
  probeBindings(self)[[NSString stringWithFormat:@"%sbytes%lu",probeStage(stage),(unsigned long)index]]=probeHex(bytes,length,768);
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];""")
edit('NativeBridgeMetalFrontend.mm',
     """   [targets addObject:target];
  }
 }""",
     """   [targets addObject:target];
  }
  probeBindings(self)[@"armed"]=[NSString stringWithFormat:@"%lux%lu f%lu at %s%lu",(unsigned long)texture.width,
   (unsigned long)texture.height,(unsigned long)texture.pixelFormat,probeStage(stage),(unsigned long)index];
 }""")

# ---- data-a-live: the App Library draw's whole state, for a replay (bb36) -------------------------------------
# With the probe, every binding an encoder makes is remembered (pipeline with its functions' constants and blend
# state, buffers with their first bytes, bytes, samplers, textures); the first draw after a probed texture is bound
# prints it all with the draw's own arguments and, for an indexed draw, its indices. mtltest replays it from this.
edit('NativeBridgeMetalFrontend.mm',
     """ objc_setAssociatedObject(state,&gCapNamesKey,[NSString stringWithFormat:@"%@ / %@ c0 f%lu blend %d",d.vertexFunction.name,d.fragmentFunction.name,
  (unsigned long)d.colorAttachments[0].pixelFormat,(int)d.colorAttachments[0].blendingEnabled],OBJC_ASSOCIATION_RETAIN_NONATOMIC);""",
     """ objc_setAssociatedObject(state,&gCapNamesKey,[NSString stringWithFormat:@"%@ / %@ c0 f%lu blend %d",d.vertexFunction.name,d.fragmentFunction.name,
  (unsigned long)d.colorAttachments[0].pixelFormat,(int)d.colorAttachments[0].blendingEnabled],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(pandoraAstcProbe()) {   // data-a-live: the whole pipeline, for the probe's draw log
  NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
  NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[0];
  NSMutableString *full=[NSMutableString stringWithFormat:@"v %@ {%s} f %@ {%s} c0 %lu mask %lu blend %d %lu/%lu/%lu %lu/%lu/%lu c1 %lu mask %lu blend %d samples %lu",
   d.vertexFunction.name,vf?vf.constantsText.c_str():"",d.fragmentFunction.name,ff?ff.constantsText.c_str():"",(unsigned long)a.pixelFormat,
   (unsigned long)a.writeMask,(int)a.blendingEnabled,(unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,
   (unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,(unsigned long)a.destinationAlphaBlendFactor,
   (unsigned long)a.alphaBlendOperation,(unsigned long)d.colorAttachments[1].pixelFormat,(unsigned long)d.colorAttachments[1].writeMask,
   (int)d.colorAttachments[1].blendingEnabled,(unsigned long)d.rasterSampleCount];
  if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *at=v.attributes[i];if(at.format==MTLVertexFormatInvalid)continue;
   [full appendFormat:@" a%u=%lu:%lu:%lu:%lu",i,(unsigned long)at.format,(unsigned long)at.offset,(unsigned long)at.bufferIndex,
    (unsigned long)v.layouts[at.bufferIndex<31?at.bufferIndex:0].stride];
  }
  objc_setAssociatedObject(state,&gProbePipeKey,full,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 }""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"pipe %@ %@",capId(state),objc_getAssociatedObject(state,&gCapNamesKey)]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"pipe %@ %@",capId(state),objc_getAssociatedObject(state,&gCapNamesKey)]);
 if(pandoraAstcProbe())probeBindings(self)[@"pipe"]=objc_getAssociatedObject(state,&gProbePipeKey)?:@"?";
""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 [self check:nmb_encoder_set_buffer""",
     """ if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log, with the first bytes from the offset
  const uint8_t *contents=buffer?(const uint8_t *)buffer.contents:nullptr;
  probeBindings(self)[[NSString stringWithFormat:@"%sbuffer%lu",probeStage(stage),(unsigned long)index]]=!buffer?@"none":
   [NSString stringWithFormat:@"len %lu off %lu %@",(unsigned long)buffer.length,(unsigned long)offset,
    contents && offset<buffer.length?probeHex(contents+offset,buffer.length-offset,768):@"(no contents)"];
 }
 [self check:nmb_encoder_set_buffer""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"tex %s%lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,capTexture(texture)]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"tex %s%lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,capTexture(texture)]);
 if(pandoraAstcProbe())probeBindings(self)[[NSString stringWithFormat:@"%stex%lu",probeStage(stage),(unsigned long)index]]=capTexture(texture);
""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"drawidx p%lu n%lu i%lu",(unsigned long)t,(unsigned long)n,(unsigned long)i]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"drawidx p%lu n%lu i%lu",(unsigned long)t,(unsigned long)n,(unsigned long)i]);
 if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   const uint8_t *ic=b?(const uint8_t *)b.contents:nullptr;const NSUInteger isz=k==MTLIndexTypeUInt16?2:4;
   NSLog(@"pandora-metal replay %d: drawIndexed prim %lu count %lu type %lu offset %lu instances %lu baseVertex %ld indices %@",nth,
    (unsigned long)t,(unsigned long)n,(unsigned long)k,(unsigned long)o,(unsigned long)i,(long)v,
    ic && o<b.length?probeHex(ic+o,MIN(n*isz,b.length-o),96):@"(no contents)");
   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])probeLog([NSString stringWithFormat:@"%d",nth],key,seen[key]);
  }
  [probeBindings(self) removeObjectForKey:@"armed"];
 }
""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"draw p%lu v%lu+%lu i%lu",(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"draw p%lu v%lu+%lu i%lu",(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i]);
 if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   NSLog(@"pandora-metal replay d%d: draw prim %lu start %lu count %lu instances %lu",nth,(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i);
   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])probeLog([NSString stringWithFormat:@"d%d",nth],key,seen[key]);
  }
  [probeBindings(self) removeObjectForKey:@"armed"];
 }
""")

# ---- data-a-live: the probed draw's viewport, scissor and pass (bb38) -----------------------------------------
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"viewport %.1f %.1f %.1fx%.1f",v.originX,v.originY,v.width,v.height]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"viewport %.1f %.1f %.1fx%.1f",v.originX,v.originY,v.width,v.height]);
 if(pandoraAstcProbe())probeBindings(self)[@"viewport"]=[NSString stringWithFormat:@"%g %g %gx%g z %g-%g",v.originX,v.originY,v.width,v.height,v.znear,v.zfar];
""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(capturing())capture([NSString stringWithFormat:@"scissor %lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height]);
""",
     """ if(capturing())capture([NSString stringWithFormat:@"scissor %lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height]);
 if(pandoraAstcProbe())probeBindings(self)[@"scissor"]=[NSString stringWithFormat:@"%lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height];
""")
edit('NativeBridgeMetalFrontend.mm',
     """  objc_setAssociatedObject(made,&gProbePassTargetKey,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
""",
     """  objc_setAssociatedObject(made,&gProbePassTargetKey,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(made && pandoraAstcProbe()) {   // data-a-live: the pass, for the probe's draw log
  NSMutableString *pass=[NSMutableString stringWithFormat:@"rt %lux%lu",(unsigned long)d.renderTargetWidth,(unsigned long)d.renderTargetHeight];
  for(unsigned i=0;i<8;i++) {
   MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];if(!c.texture)continue;
   [pass appendFormat:@" | c%u %@ load%lu store%lu clear %g,%g,%g,%g",i,capTexture(c.texture),(unsigned long)c.loadAction,
    (unsigned long)c.storeAction,c.clearColor.red,c.clearColor.green,c.clearColor.blue,c.clearColor.alpha];
  }
  if(d.depthAttachment.texture)[pass appendFormat:@" | depth %@",capTexture(d.depthAttachment.texture)];
  if(d.stencilAttachment.texture)[pass appendFormat:@" | stencil %@",capTexture(d.stencilAttachment.texture)];
  probeBindings(made)[@"pass"]=pass;
 }
""")

# ---- data-a-live: what the CPU wrote vs what the GPU reads, for the probed draw (bb39) ------------------------
# bb38 replayed on the PC (metal2vulkan on lavapipe, /opt/work/replay): QuartzCore's shader translates right, but the
# 64 bytes at vertex buffer 2 + 208 (its mvp matrix) as captured at setVertexBuffer are no matrix, every triangle has
# no area and nothing is drawn; a real matrix there draws the image. QuartzCore may write the matrix after binding.
# Now, for the first probed draws, each buffer bound to the draw is logged (64 bytes from its offset) at the draw, at
# commit and after completion from the CPU, and after completion as the GPU reads it (a blit into a fresh buffer), with
# what kind of buffer it is. The texture's own descriptor (usage, storage, swizzle) is logged where it is made.
edit('NativeBridgeMetalFrontend.mm',
     """#include <sys/sysctl.h>
static int pandoraAstcProbeLevel""",
     """#include <sys/sysctl.h>
#include <objc/message.h>
static int pandoraAstcProbeLevel""")
edit('NativeBridgeMetalFrontend.mm',
     """static const char *probeStage(uint32_t stage) {return stage==1?"v":stage==2?"f":"c";}""",
     """static const char *probeStage(uint32_t stage) {return stage==1?"v":stage==2?"f":"c";}
static char gProbeWatchKey;
static NSString *probeBufferKind(id buffer) {
 NSMutableString *s=[NSMutableString stringWithFormat:@"len %lu storage %lu",(unsigned long)[(id<MTLBuffer>)buffer length],
  (unsigned long)[(id<MTLBuffer>)buffer storageMode]];
 SEL wrapped=NSSelectorFromString(@"wrapped");
 if([buffer respondsToSelector:wrapped] && ((void *(*)(id,SEL))objc_msgSend)(buffer,wrapped))[s appendString:@" no-copy"];
 if([(id<MTLBuffer>)buffer heap])[s appendString:@" heap"];
 return s;
}""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log, with the first bytes from the offset
""",
     """ if(pandoraAstcProbe())   // data-a-live: the buffer itself, for the probe's CPU/GPU comparison
  probeBindings(self)[[NSString stringWithFormat:@"~%sbuffer%lu",probeStage(stage),(unsigned long)index]]=buffer?@[buffer,@(offset)]:@[];
 if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log, with the first bytes from the offset
""")
edit('NativeBridgeMetalFrontend.mm',
     """   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])probeLog([NSString stringWithFormat:@"%d",nth],key,seen[key]);
  }""",
     """   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])
    if(![key hasPrefix:@"~"])probeLog([NSString stringWithFormat:@"%d",nth],key,seen[key]);
   // the buffers as the CPU holds them now; watched again at commit and after completion (the GPU's view too)
   NSMutableArray *watch=objc_getAssociatedObject(self.commandBuffer,&gProbeWatchKey);
   if(!watch){watch=[NSMutableArray array];objc_setAssociatedObject(self.commandBuffer,&gProbeWatchKey,watch,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   for(NSString *key in seen)if([key hasPrefix:@"~"] && [seen[key] count]==2) {
    id<MTLBuffer> buffer=seen[key][0];const NSUInteger offset=[seen[key][1] unsignedLongValue];
    const uint8_t *c=(const uint8_t *)buffer.contents;
    NSString *label=[NSString stringWithFormat:@"%d %@+%lu",nth,[key substringFromIndex:1],(unsigned long)offset];
    NSLog(@"pandora-metal watch %@ at draw: %@ | %@",label,probeBufferKind(buffer),c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,64):@"(no contents)");
    [watch addObject:@[buffer,@(offset),label]];
   }
  }""")
edit('NativeBridgeMetalFrontend.mm',
     """  flushSurfaces(_surfaces);
  flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
  // Completion owns this reference""",
     """  flushSurfaces(_surfaces);
  flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
  for(NSArray *w in objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: the probe's buffers at commit
   id<MTLBuffer> buffer=w[0];const NSUInteger offset=[w[1] unsignedLongValue];const uint8_t *c=(const uint8_t *)buffer.contents;
   // bb39: QuartzCore writes these buffers after binding them; what the GPU reads is what they hold at commit, so the
   // replay takes its bytes from here (1 KiB from the offset, in pieces).
   probeLog([NSString stringWithFormat:@"commit %@",w[2]],@"bytes",c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,1024):@"(no contents)");
  }
  // Completion owns this reference""")
edit('NativeBridgeMetalFrontend.mm',
     """ @synchronized(gInFlight){[gInFlight removeObject:self];}
 if(NSSet *targets=objc_getAssociatedObject(self,&gProbeReadbackKey)) {""",
     """ @synchronized(gInFlight){[gInFlight removeObject:self];}
 if(NSArray *watch=objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: after completion, CPU and GPU views
  id<MTLCommandQueue> queue=(id<MTLCommandQueue>)self.queue;
  for(NSArray *w in watch) {
   id<MTLBuffer> buffer=w[0];const NSUInteger offset=[w[1] unsignedLongValue];NSString *label=w[2];
   const uint8_t *c=(const uint8_t *)buffer.contents;
   NSLog(@"pandora-metal watch %@ done, CPU: %@",label,c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,64):@"(no contents)");
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
    const NSUInteger n=MIN((NSUInteger)64,buffer.length-offset);
    id<MTLBuffer> copy=[buffer.device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    memset(copy.contents,0xa5,64);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromBuffer:buffer sourceOffset:offset toBuffer:copy destinationOffset:0 size:n];
    [b endEncoding];[cb commit];[cb waitUntilCompleted];
    NSLog(@"pandora-metal watch %@ done, GPU: %@ (status %ld)",label,probeHex(copy.contents,n,64),(long)cb.status);
   });
  }
 }
 if(NSSet *targets=objc_getAssociatedObject(self,&gProbeReadbackKey)) {""")
edit('NativeBridgeMetalFrontend.mm',
     """   if(probe && bb==16 && d.width>=512) {""",
     """   if(probe && bb==16 && d.width>=512)   // data-a-live: QuartzCore's own descriptor of the image (bb39)
    NSLog(@"pandora-metal astc descriptor: usage %lu storage %lu cpu cache %lu hazard %lu resource options %lu swizzle %u/%u/%u/%u "
          "type %lu mips %lu samples %lu allow gpu opt %d | bytes %p length %lu",(unsigned long)d.usage,(unsigned long)d.storageMode,
          (unsigned long)d.cpuCacheMode,(unsigned long)d.hazardTrackingMode,(unsigned long)d.resourceOptions,(unsigned)d.swizzle.red,
          (unsigned)d.swizzle.green,(unsigned)d.swizzle.blue,(unsigned)d.swizzle.alpha,(unsigned long)d.textureType,
          (unsigned long)d.mipmapLevelCount,(unsigned long)d.sampleCount,(int)d.allowGPUOptimizedContents,
          bytes,(unsigned long)length);
   if(probe && bb==16 && d.width>=512) {""")

# ---- data-a-live: did the App Library draw make any fragment? (bb40) -------------------------------------------
# QuartzCore's fragment function forces the sampled alpha to 1, yet the App Library's target holds transparent black
# where the image goes: the draw makes no fragment, or one of its inputs scales the colour to nothing. With the probe,
# every render pass without its own visibility buffer gets one, and from the probed draw on each draw of that encoder
# counts its samples into its own slot (Metal's counting visibility results), logged at completion with the pipeline
# of each slot. The raster state (cull, winding, fill), the depth/stencil state and the index buffer at commit join
# the replay log.
edit('NativeBridgeMetalFrontend.mm',
     """static char gProbeWatchKey;""",
     """static char gProbeWatchKey,gProbeVisKey,gProbeVisLabelsKey,gProbeDepthKey;""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]])""",
     """ NBBuffer *probeVisibility=nil;   // data-a-live: a visibility buffer of the probe's own for this pass
 if(pandoraAstcProbe() && !d.visibilityResultBuffer && gDeviceObject) {
  probeVisibility=(NBBuffer *)[(id<MTLDevice>)gDeviceObject newBufferWithLength:64*8 options:MTLResourceStorageModeShared];
  if(probeVisibility) {
   memset(probeVisibility.contents,0,64*8);
   [_resourceOwners addObject:probeVisibility];
   pass.visibility=probeVisibility.handle;
  }
 }
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]])""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(made && pandoraAstcProbe()) {   // data-a-live: the pass, for the probe's draw log
""",
     """ if(made && probeVisibility)objc_setAssociatedObject(made,&gProbeVisKey,probeVisibility,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(made && pandoraAstcProbe()) {   // data-a-live: the pass, for the probe's draw log
""")
# depth/stencil states keep their descriptor's description; the encoder remembers the bound one and the raster state
edit('NativeBridgeMetalFrontend.mm',
     """ NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;return (id<MTLDepthStencilState>)state;""",
     """ NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;
 if(pandoraAstcProbe())objc_setAssociatedObject(state,&gProbeDepthKey,[NSString stringWithFormat:@"depth compare %lu write %d stencil front %lu/%lu/%lu/%lu back %lu/%lu/%lu/%lu",
  (unsigned long)d.depthCompareFunction,(int)d.depthWriteEnabled,(unsigned long)d.frontFaceStencil.stencilCompareFunction,
  (unsigned long)d.frontFaceStencil.stencilFailureOperation,(unsigned long)d.frontFaceStencil.depthFailureOperation,
  (unsigned long)d.frontFaceStencil.depthStencilPassOperation,(unsigned long)d.backFaceStencil.stencilCompareFunction,
  (unsigned long)d.backFaceStencil.stencilFailureOperation,(unsigned long)d.backFaceStencil.depthFailureOperation,
  (unsigned long)d.backFaceStencil.depthStencilPassOperation],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return (id<MTLDepthStencilState>)state;""")
edit('NativeBridgeMetalFrontend.mm',
     """ [self check:nmb_render_set_depth_stencil(self.handle,""",
     """ if(pandoraAstcProbe())probeBindings(self)[@"depthstencil"]=state?(objc_getAssociatedObject(state,&gProbeDepthKey)?:@"?"):@"none";
 [self check:nmb_render_set_depth_stencil(self.handle,""")
edit('NativeBridgeMetalFrontend.mm',
     """ [self check:nmb_render_set_raster(self.handle,""",
     """ if(pandoraAstcProbe())probeBindings(self)[@"raster"]=[NSString stringWithFormat:@"cull %llu winding %llu fill %llu",
  cull.unsignedLongLongValue,winding.unsignedLongLongValue,fill.unsignedLongLongValue];
 [self check:nmb_render_set_raster(self.handle,""")
# counting: the probed draw takes slot 0, every later draw of the encoder the next slot
edit('NativeBridgeMetalFrontend.mm',
     """ if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   const uint8_t *ic=""",
     """ if(pandoraAstcProbe() && objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: count this draw's samples
  NSMutableArray *labels=objc_getAssociatedObject(self,&gProbeVisLabelsKey);
  if(probeBindings(self)[@"armed"] && !labels) {
   labels=[NSMutableArray array];objc_setAssociatedObject(self,&gProbeVisLabelsKey,labels,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   NSMutableArray *reads=objc_getAssociatedObject(self.commandBuffer,&gProbeVisKey);
   if(!reads){reads=[NSMutableArray array];objc_setAssociatedObject(self.commandBuffer,&gProbeVisKey,reads,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   [reads addObject:@[objc_getAssociatedObject(self,&gProbeVisKey),labels]];
  }
  if(labels && labels.count<64) {
   [labels addObject:[NSString stringWithFormat:@"%@pipe %@",probeBindings(self)[@"armed"]?@"PROBED ":@"",probeBindings(self)[@"pipeid"]?:@"?"]];
   [(id<MTLRenderCommandEncoder>)self setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(labels.count-1)*8];
  }
 }
 if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   const uint8_t *ic=""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   NSLog(@"pandora-metal replay d%d""",
     """ if(pandoraAstcProbe() && objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: count this draw's samples
  NSMutableArray *labels=objc_getAssociatedObject(self,&gProbeVisLabelsKey);
  if(labels && labels.count<64) {
   [labels addObject:[NSString stringWithFormat:@"%@pipe %@",probeBindings(self)[@"armed"]?@"PROBED ":@"",probeBindings(self)[@"pipeid"]?:@"?"]];
   [(id<MTLRenderCommandEncoder>)self setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(labels.count-1)*8];
  }
 }
 if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<8) {
   NSLog(@"pandora-metal replay d%d""")
# the index buffer at commit, with the other watched buffers
edit('NativeBridgeMetalFrontend.mm',
     """   for(NSString *key in seen)if([key hasPrefix:@"~"] && [seen[key] count]==2) {""",
     """   if(b)[watch addObject:@[b,@(o),[NSString stringWithFormat:@"%d index+%lu",nth,(unsigned long)o]]];
   for(NSString *key in seen)if([key hasPrefix:@"~"] && [seen[key] count]==2) {""")
# at completion: the counts
edit('NativeBridgeMetalFrontend.mm',
     """ if(NSArray *watch=objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: after completion, CPU and GPU views""",
     """ static std::atomic<int> visTold{0};
 for(NSArray *read in objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: samples counted per draw
  if(visTold.fetch_add(1)>=8)break;
  const uint64_t *counts=(const uint64_t *)[(id<MTLBuffer>)read[0] contents];NSArray *labels=read[1];
  for(NSUInteger i=0;i<labels.count && counts;i++)
   probeLog([NSString stringWithFormat:@"visibility %d slot %lu",visTold.load(),(unsigned long)i],
            [NSString stringWithFormat:@"%llu samples",(unsigned long long)counts[i]],labels[i]);
 }
 if(NSArray *watch=objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: after completion, CPU and GPU views""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(pandoraAstcProbe())probeBindings(self)[@"pipe"]=objc_getAssociatedObject(state,&gProbePipeKey)?:@"?";
""",
     """ if(pandoraAstcProbe())probeBindings(self)[@"pipe"]=objc_getAssociatedObject(state,&gProbePipeKey)?:@"?";
 if(pandoraAstcProbe())probeBindings(self)[@"pipeid"]=capId(state);
""")


# ---- data-a-live: the draws after the probed one too (bb41) --------------------------------------------------
# bb40: the App Library's ASTC draws make every fragment (1125x1046 samples each), and the next draws of the same
# pass (two of b2a8f, blending off, then b7854) cover the same area: they leave it transparent. Every draw after the
# probed one in its encoder now goes into the replay log with its whole state and its buffers at commit, so the pass
# can be replayed in order on the PC.
def edit_all(name, old, new, count):
    p = ROOT / name
    s = p.read_text()
    assert s.count(old) == count, (name, old[:70], s.count(old))
    p.write_text(s.replace(old, new))
    print('edited', name)


edit_all('NativeBridgeMetalFrontend.mm',
         """[probeBindings(self) removeObjectForKey:@"armed"];""",
         """[probeBindings(self) removeObjectForKey:@"armed"];probeBindings(self)[@"following"]=@YES;""", 2)
edit_all('NativeBridgeMetalFrontend.mm',
         """ if(pandoraAstcProbe() && probeBindings(self)[@"armed"]) {   // data-a-live: the probed draw's whole state""",
         """ if(pandoraAstcProbe() && (probeBindings(self)[@"armed"] || probeBindings(self)[@"following"])) {   // data-a-live: the probed draw's whole state, and the draws after it""", 2)
edit_all('NativeBridgeMetalFrontend.mm', """  if(nth<8) {""", """  if(nth<48) {""", 2)

# ---- data-a-live: buffer offsets moved after binding (bb42) --------------------------------------------------
# bb41 replayed on the PC: the draw after the App Library's ASTC draws (b2a8f) fills its quad with its vertex colour,
# blending off, as Apple's own IR says; which quad and colour depends on where its vertices are. QuartzCore moves the
# bound vertex buffer with setVertexBufferOffset between draws, which the replay log did not see. The probe's bindings
# and its commit-time watch now follow such moves.
edit('NativeBridgeMetalFrontend.mm',
     """- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
""",
     """- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(pandoraAstcProbe()) {   // data-a-live: the binding moves; the probe's draw log and watch follow it
  NSMutableDictionary *seen=probeBindings(self);
  NSString *key=[NSString stringWithFormat:@"~%sbuffer%lu",probeStage(stage),(unsigned long)index];
  NSArray *bound=seen[key];
  if(bound.count==2)seen[key]=@[bound[0],@(offset)];
  seen[[NSString stringWithFormat:@"%sbufferoffset%lu",probeStage(stage),(unsigned long)index]]=@(offset);
 }
""")

# ---- data-a-live: 'b3a8' surfaces whose planes report no row bytes (bb43) -------------------------------------
# bb43: QuartzCore's own App Library draws render right on the phone (mtltest part 9). What stays empty is a layer
# drawn from a full-screen two-plane extended-range surface ('b3a8', RGB10A8_2P_XR10): the frontend refused it every
# time (step 3) because IOSurfaceGetBytesPerRowOfPlane answers 0 for it here. Its allocation is exactly two linear
# planes, rows aligned to 64 bytes, one after the other, rounded to 4 KiB: 1125x2436 -> 4544 * 2436 + 1152 * 2436 =
# 13875456 -> 13877248, 1065x282 -> 1519616, as logged. Those planes are now computed when the surface gives none.
edit('NativeBridgeMetalFrontend.mm',
     """static NBTexture *textureOnSurface(NBBuffer *buffer,""",
     """// data-a-live: a 'b3a8' surface's planes when it reports none (bb43): {offset0, row0, offset1, row1}, or false.
static bool pandoraB3a8Planes(IOSurfaceRef surface,size_t allocation,size_t planes[4]) {
 if(!surface || IOSurfaceGetPixelFormat(surface)!=0x62336138 || IOSurfaceGetPlaneCount(surface)!=2)return false;
 if(IOSurfaceGetBytesPerRowOfPlane(surface,0) && IOSurfaceGetBytesPerRowOfPlane(surface,1))return false;
 const size_t w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface);
 const size_t row0=(w*4+63)&~(size_t)63,row1=(w+63)&~(size_t)63;
 if(!w || !h || row0*h+row1*h>allocation)return false;
 planes[0]=0;planes[1]=row0;planes[2]=row0*h;planes[3]=row1;
 static std::atomic<int> told{0};
 if(told.fetch_add(1)<8)NSLog(@"pandora-metal b3a8 %lux%lu: planes computed (row %lu at 0, row %lu at %lu; %lu of %lu bytes)",(unsigned long)w,
  (unsigned long)h,(unsigned long)row0,(unsigned long)row1,(unsigned long)(row0*h),(unsigned long)(row0*h+row1*h),(unsigned long)allocation);
 return true;
}
static NBTexture *textureOnSurface(NBBuffer *buffer,""")
edit('NativeBridgeMetalFrontend.mm',
     """ const uint64_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface),chroma=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,1);
 NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,IOSurfaceGetPixelFormat(surface),plane,
  IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chroma>=base?chroma-base:0,IOSurfaceGetBytesPerRowOfPlane(surface,1));""",
     """ const uint64_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface),chroma=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,1);
 uint64_t chromaOffset=chroma>=base?chroma-base:0,chromaRow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
 size_t computed[4];   // data-a-live: a 'b3a8' surface without plane rows (bb43)
 if(!chromaRow && pandoraB3a8Planes(surface,IOSurfaceGetAllocSize(surface),computed)){chromaOffset=computed[2];chromaRow=computed[3];}
 NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,IOSurfaceGetPixelFormat(surface),plane,
  IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chromaOffset,chromaRow);""")
edit('NativeBridgeMetalFrontend.mm',
     """ const uintptr_t start=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 const size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
""",
     """ uintptr_t start=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
 size_t computed[4];   // data-a-live: a 'b3a8' surface without plane rows (bb43)
 if(!row && plane<2 && pandoraB3a8Planes(surface,allocation,computed)){start=(uintptr_t)base+computed[plane*2];row=computed[plane*2+1];}
""")

# ---- data-a-live: where the bridge's CPU time goes, and how Turnip draws (2026-10-25, round 1) -----------------
# While the screen moves backboardd uses up to two thirds of a core, its render thread almost half (app1). Every call
# into the bridge (metal2vulkan's runtime and Turnip below it, on the caller's thread) is now timed: per bridge
# function its calls and time, and the time inside the bridge at all (a call inside a call counted once; the
# blocking waits apart). backboardd logs it every 10 s for 10 minutes as "pandora-metal B", the busiest functions
# first, with Turnip's render pass counts (GMEM or sysmem and Turnip's reason, tools/turnip/pandora_rpstats.py).
# Counting only: no call changes. PandoraBridgeTiming.h is generated from the bridge's header: one macro per
# function wraps each call in the frontend in a timed lambda.
import re

names = sorted(set(re.findall(r'^[A-Za-z_][A-Za-z0-9_ *]*?\b(nmb_[a-z0-9_]+)\(',
                              (ROOT / 'bridge/NativeMetalObjects.h').read_text(), re.M)))
assert len(names) > 60, len(names)
front = (ROOT / 'NativeBridgeMetalFrontend.mm').read_text()
for n in names:
    assert not re.search(r'^[A-Za-z].*\b%s\([^;]*\)\s*\{' % n, front, re.M), ('defined in the frontend', n)
waits = [n for n in names if '_wait' in n]
(ROOT / 'PandoraBridgeTiming.h').write_text(
    '// Generated by tools/metal/adapt_ios16.py (data-a-live): every bridge call in the frontend counted, 1 in 16 timed.\n'
    '#pragma once\n#include <atomic>\n#include <time.h>\n'
    'enum {' + ','.join('PANDORA_BT_' + n for n in names) + ',PANDORA_BT_COUNT};\n'
    'static const char *const gPandoraBridgeNames[PANDORA_BT_COUNT]={' + ','.join('"%s"' % n for n in names) + '};\n'
    'static const bool gPandoraBridgeWait[PANDORA_BT_COUNT]={' + ','.join('true' if n in waits else 'false' for n in names) + '};\n'
    'struct PandoraBridgeSlot {std::atomic<uint64_t> calls{0},ns{0};};\n'
    'static PandoraBridgeSlot gPandoraBridge[PANDORA_BT_COUNT];\n'
    'static std::atomic<uint64_t> gPandoraBridgeOuter{0},gPandoraBridgeWaits{0};\n'
    '// Every call counted; one in PANDORA_BT_EVERY timed, picked at random (xorshift32, so no pattern of calls always\n'
    '// lands on the same one) and counted PANDORA_BT_EVERY times; the blocking waits, few and long, all timed.\n'
    'enum {PANDORA_BT_EVERY=16};\n'
    'struct PandoraBridgeThread {unsigned depth;uint32_t dice;};\n'
    'static thread_local PandoraBridgeThread gPandoraBridgeThread{0,0x9e3779b9u};\n'
    'template<class F> static inline decltype(auto) pandoraTimed(unsigned slot,F &&f) {\n'
    ' PandoraBridgeThread &me=gPandoraBridgeThread;\n'
    ' gPandoraBridge[slot].calls.fetch_add(1,std::memory_order_relaxed);\n'
    ' uint32_t x=me.dice;x^=x<<13;x^=x>>17;x^=x<<5;me.dice=x;\n'
    ' const bool wait=gPandoraBridgeWait[slot];\n'
    ' struct End {PandoraBridgeThread &me;unsigned slot;bool wait;uint64_t t0;~End(){const unsigned depth=--me.depth;if(!t0)return;\n'
    '  const uint64_t d=(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-t0)*(wait?1:PANDORA_BT_EVERY);\n'
    '  gPandoraBridge[slot].ns.fetch_add(d,std::memory_order_relaxed);\n'
    '  if(depth==0)(wait?gPandoraBridgeWaits:gPandoraBridgeOuter).fetch_add(d,std::memory_order_relaxed);}};\n'
    ' ++me.depth;\n'
    ' End end{me,slot,wait,wait || (x&(PANDORA_BT_EVERY-1))==0?clock_gettime_nsec_np(CLOCK_UPTIME_RAW):0};\n'
    ' return f();\n'
    '}\n'
    + ''.join('#define %s(...) pandoraTimed(PANDORA_BT_%s,[&]() -> decltype(auto) {return %s(__VA_ARGS__);})\n' % (n, n, n)
              for n in names))
print('wrote PandoraBridgeTiming.h,', len(names), 'functions,', len(waits), 'waits')
edit('NativeBridgeMetalFrontend.mm',
     """#include "NativeBridgeMetalFrontend.h"
""",
     """#include "NativeBridgeMetalFrontend.h"
#include "PandoraBridgeTiming.h"   // data-a-live: every bridge call counted, 1 in 16 timed (tools/metal/adapt_ios16.py)
""")
p = ROOT / 'NativeBridgeMetalFrontend.mm'
p.write_text(p.read_text() + r'''
// data-a-live: the bridge's time since the last call, as text (tools/metal/adapt_ios16.py, 2026-10-25): the share of
// one CPU inside the bridge and in its blocking waits, then the busiest functions; then Turnip's render pass counts.
extern "C" size_t native_bridge_metal_pandora_timing(char *out,size_t size) {
 static uint64_t last,lastOuter,lastWaits,shownCalls[PANDORA_BT_COUNT],shownNs[PANDORA_BT_COUNT];
 const uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW),span=last?now-last:0;
 last=now;
 const uint64_t outer=gPandoraBridgeOuter.load(),waits=gPandoraBridgeWaits.load();
 const uint64_t dOuter=outer-lastOuter,dWaits=waits-lastWaits;
 lastOuter=outer;lastWaits=waits;
 uint64_t calls[PANDORA_BT_COUNT],ns[PANDORA_BT_COUNT];
 unsigned order[PANDORA_BT_COUNT];
 for(unsigned i=0;i<PANDORA_BT_COUNT;i++){
  const uint64_t c=gPandoraBridge[i].calls.load(),t=gPandoraBridge[i].ns.load();
  calls[i]=c-shownCalls[i];ns[i]=t-shownNs[i];shownCalls[i]=c;shownNs[i]=t;order[i]=i;
 }
 std::sort(order,order+PANDORA_BT_COUNT,[&](unsigned a,unsigned b){return ns[a]>ns[b];});
 int at=snprintf(out,size,"bridge %llu.%llu%% of one CPU over %llu ms, waits %llu ms (1 call in 16 timed)\n",
  (unsigned long long)(span?dOuter*100/span:0),(unsigned long long)(span?dOuter*1000/span%10:0),
  (unsigned long long)(span/1000000),(unsigned long long)(dWaits/1000000));
 for(unsigned k=0;k<14 && at>0 && (size_t)at<size && ns[order[k]];k++){
  const unsigned i=order[k];
  at+=snprintf(out+at,size-at,"  %s: %llu calls, %llu us (%llu ns each)\n",gPandoraBridgeNames[i],(unsigned long long)calls[i],
   (unsigned long long)(ns[i]/1000),(unsigned long long)(calls[i]?ns[i]/calls[i]:0));
 }
 static size_t (*stats)(char *,size_t);
 static bool looked;
 if(!looked){
  looked=true;
  for(const char *path:{"/usr/lib/libvulkan_freedreno.dylib",(const char *)nullptr}){
   void *image=path?dlopen(path,RTLD_NOLOAD|RTLD_LAZY):RTLD_DEFAULT;
   if(image && (stats=(size_t (*)(char *,size_t))dlsym(image,"tu_pandora_render_stats")))break;
  }
 }
 if(at>0 && (size_t)at<size){
  if(stats){at+=snprintf(out+at,size-at,"turnip: ");if((size_t)at<size)at+=(int)stats(out+at,size-at);}
  else at+=snprintf(out+at,size-at,"turnip: no render pass counts (tu_pandora_render_stats not found)");
 }
 return at>0?std::min((size_t)at,size?size-1:0):0;
}
''')
print('edited NativeBridgeMetalFrontend.mm (timing report)')
edit('NativeAdrenoMetalPlugin.mm',
     """#include <sys/sysctl.h>
""",
     """#include <sys/sysctl.h>
extern "C" size_t native_bridge_metal_pandora_timing(char *out, size_t size);   // data-a-live (NativeBridgeMetalFrontend.mm)
""")
edit('NativeAdrenoMetalPlugin.mm',
     """        NSLog(@"pandora-metal: backboardd has a device; CAEnableDeepFramebuffer %@", (__bridge id)deep);
""",
     """        NSLog(@"pandora-metal: backboardd has a device; CAEnableDeepFramebuffer %@", (__bridge id)deep);
        pthread_t timing;   // data-a-live: the bridge's time and Turnip's render passes, every 10 s for 10 minutes
        if (pthread_create(&timing, nullptr, [](void *) -> void * {
                static char text[4096];
                native_bridge_metal_pandora_timing(text, sizeof text);   // starts the first interval
                for (unsigned round = 0; round < 60; ++round) {
                    sleep(10);
                    native_bridge_metal_pandora_timing(text, sizeof text);
                    for (char *line = text, *end; *line; line = end) {
                        end = strchr(line, '\\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal B%u %s", round, line);
                    }
                }
                return nullptr;
            }, nullptr) == 0) pthread_detach(timing);
""")

# ---- data-a-live: device answers asked once per (selector, argument) (2026-10-25, round 2) ----------------------
# r1c: QuartzCore asks the device about 3100 questions with an argument every 10 s while the screen moves
# (supportsFamily:, supportsTextureSampleCount:, minimumLinearTextureAlignmentForPixelFormat:, ...), and the runtime
# worked each out again: 140 us a call, 4 % of a core. Every such answer is a property of the device (NativeMetalObjects.rs
# device_answer reads only the device's properties, family rows and format table), so each is asked once and kept, the
# way the answers without an argument already are; a refusal is kept as 0/NO and logged once.
edit('NativeBridgeMetalFrontend.mm',
     """static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());return 0;}
 return value;
}""",
     """static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 static std::mutex lock;   // data-a-live: each (selector, argument) asked once (tools/metal/adapt_ios16.py, round 2)
 static std::unordered_map<uint64_t,std::vector<std::pair<SEL,uint64_t>>> known;   // argument -> (selector, answer)
 {std::lock_guard<std::mutex> hold(lock);
  if(auto at=known.find(arg);at!=known.end())for(const auto &seen:at->second)if(seen.first==cmd)return seen.second;}
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());value=0;}
 std::lock_guard<std::mutex> hold(lock);
 known[arg].emplace_back(cmd,value);
 return value;
}""")
