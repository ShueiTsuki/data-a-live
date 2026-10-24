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
     """ (void)deallocator;
 NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of raw memory is not known)");
 // ios-pandora: what is asked for (the App Library renders black, bb22): the descriptor, the memory and the
 // callers (image + offset), the first 20 calls and then one in 100.
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
