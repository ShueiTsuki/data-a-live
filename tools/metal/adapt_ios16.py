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
     """ if(!base || class_getInstanceSize(base)!=536){""",
     """ // ios-pandora: 528 bytes on iOS 16.7 (ios-fuxi docs-lanes/metal.md), 536 on iPadOS 17.7.11.
 if(!base || (class_getInstanceSize(base)!=536 && class_getInstanceSize(base)!=528)){""")
edit('NativeTurnipSmoke.h',
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    return true;""",
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    if (access(path, R_OK) != 0) snprintf(path, size, "%s", "/usr/lib/libvulkan_freedreno.dylib");   // ios-pandora
    return true;""")
