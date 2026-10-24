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

# ---- ios-pandora: what the App Library's ASTC bytes look like (bb28) ------------------------------------------
# Re-uploading at commit (38391ce) never fired for the App Library (its bytes do not change after the texture is
# made) and froze backboardd in the app switcher, so it is gone. The image still shows a sharp picture in one part
# and black elsewhere. For a block-compressed texture the next lines describe the bytes as rows of blocks: an 8x8
# grid of the image, each cell the share of all-zero blocks (z), ASTC void-extent (single colour) blocks (v) and
# other blocks (o), so the picture's layout can be compared with the screen, and the callers.
edit('NativeBridgeMetalFrontend.mm',
     """   static std::atomic<int> captures{0};""",
     """   if(bb==16) {   // ios-pandora: the block layout of the bytes, 8x8 grid
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

# ---- ios-pandora: no-copy memory is cleaned like surfaces (bb30) ---------------------------------------------
# The kernel maps imported user memory for the GPU uncached and leaves the CPU's caches to the importer
# (NativeAdrenoABI.h; pandora_gpu.c flushes every import only with pandora_gpu_flushall=1). NativeMetal cleans
# the IOSurfaces a command buffer uses, but not memory from newBufferWithBytesNoCopy or textures over it, so
# the GPU can read what the CPU has not written back yet (bb30: the Settings icon showed only its top rows).
# Now those ranges are cleaned and invalidated at commit and at completion too, as the surfaces are.
edit('NativeBridgeMetalFrontend.mm',
     """static NBTexture *textureOnBuffer(NBBuffer *buffer,""",
     """// ios-pandora: no-copy buffers a command uses, directly or under a texture (tools/metal/adapt_ios16.py).
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
 flushWrapped(_resourceOwners);   // ios-pandora: no-copy memory too
 // Actual GPU completion""")
edit('NativeBridgeMetalFrontend.mm',
     """  flushSurfaces(_surfaces);
  // Completion owns this reference""",
     """  flushSurfaces(_surfaces);
  flushWrapped(_resourceOwners);   // ios-pandora: no-copy memory too
  // Completion owns this reference""")

# ---- ios-pandora: refused IOSurface textures in the log (bb30) -----------------------------------------------
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

# ---- ios-pandora: App Library probe, boot-arg pandora_astc_probe=1 (bb31) -----------------------------------
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
     """   static const bool probe=[]{char args[1024]={};size_t size=sizeof args-1;
    return sysctlbyname("kern.bootargs",args,&size,nullptr,0)==0 && strstr(args,"pandora_astc_probe=1")!=nullptr;}();
   if(probe && bb==16 && d.width>=512) {
    std::vector<uint8_t> copy((const uint8_t *)bytes,(const uint8_t *)bytes+rowBytes*rows);
    static const uint8_t red[16]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff};
    const NSUInteger blocks=rowBytes/16;
    for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<blocks/2;x++)memcpy(&copy[y*rowBytes+x*16],red,16);
    [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:copy.data() bytesPerRow:rowBytes];
    NSLog(@"pandora-metal astc probe: %lux%lu, left %lu of %lu block columns red",(unsigned long)d.width,(unsigned long)d.height,
          (unsigned long)(blocks/2),(unsigned long)blocks);
   } else
   [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
   if(deallocator) {""")

# ---- ios-pandora: App Library readback, same boot-arg (bb32) --------------------------------------------------
# bb32: with the left halves of both ASTC images red, no red reached the screen at all; the user saw the App
# Library as always. So after the upload something is lost. With pandora_astc_probe=1 the render target a large
# ASTC texture is drawn into is now read back when that command buffer completes (off the completion thread) and
# logged as an 8x8 grid of average colours (RGBA hex): red there = the draw works and the compositing loses it.
edit('NativeBridgeMetalFrontend.mm',
     """#include <sys/sysctl.h>
""",
     """#include <sys/sysctl.h>
static bool pandoraAstcProbe() {   // ios-pandora: boot-arg pandora_astc_probe=1 (tools/metal/adapt_ios16.py)
 static const bool on=[]{char args[1024]={};size_t size=sizeof args-1;
  return sysctlbyname("kern.bootargs",args,&size,nullptr,0)==0 && strstr(args,"pandora_astc_probe=1")!=nullptr;}();
 return on;
}
static char gProbePassTargetKey,gProbeReadbackKey;
""")
edit('NativeBridgeMetalFrontend.mm',
     """ return [self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];""",
     """ id<MTLRenderCommandEncoder> made=[self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
 if(made && pandoraAstcProbe() && d.colorAttachments[0].texture)   // ios-pandora: the pass's target, for the readback
  objc_setAssociatedObject(made,&gProbePassTargetKey,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return made;""")
edit('NativeBridgeMetalFrontend.mm',
     """ if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 [self check:nmb_encoder_set_texture""",
     """ if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 if(texture && pandoraAstcProbe() && texture.pixelFormat==MTLPixelFormatASTC_4x4_LDR && texture.width>=512) {
  id target=objc_getAssociatedObject(self,&gProbePassTargetKey);   // ios-pandora: read it back at completion
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
 if(NSSet *targets=objc_getAssociatedObject(self,&gProbeReadbackKey)) {   // ios-pandora: the App Library readback
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
