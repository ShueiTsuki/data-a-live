def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0314: FairPlay's kernel driver, answering "unsupported" --------------------------------------------------------
# fairplayd.H2 opens Apple's FairPlayIOKit (com_apple_driver_FairPlayIOKit, user client
# com_apple_driver_FairPlayIOKitUserClient), which needs the SoC's FairPlay keys; our kernelcache has no such driver,
# so fairplayd exits at every start and launchd starts it again every 10 s (KeepAlive). Requests to its ports wait
# meanwhile: locationd's FairPlay signer (CLMescalSigner initInSilo:) sends MIG 1203 to com.apple.unfreed (host
# special port 17) with no timeout, its silo hangs and with it locationd's SPI service, where the Astronomy poster's
# editor waits for its location authorization (post3, post4). A driver with Apple's class names that refuses every
# call lets fairplayd run and answer its clients with FairPlay's own errors.
# In SweetPlatformExpert.cpp, a file local builds already compile: the first try put it in SweetKeyStore.cpp, which
# had only ever come from CI objects, and that kernel (post5) panicked before the USB console came up.
patch('iokit/Kernel/arm/SweetPlatformExpert.cpp', [
    ("""#include <IOKit/IOLib.h>
""",
     """#include <IOKit/IOLib.h>
#if defined(PANDORA)
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <sys/proc.h>
#endif
"""),
    ("""#endif /* SWEET */""",
     """#if defined(PANDORA)
/*
 * 0314: Apple's FairPlay driver by its class names, refusing every call. The real one
 * (com.apple.driver.FairPlayIOKit) works with keys this SoC does not have; without any, fairplayd exits at
 * start and the requests to its ports (locationd's FairPlay signer among them) wait for good. With this one
 * fairplayd runs and answers them with its own errors. The first 64 calls are logged.
 */
class com_apple_driver_FairPlayIOKit : public IOService {
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKit);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKit, IOService);

bool
com_apple_driver_FairPlayIOKit::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "com_apple_driver_FairPlayIOKitUserClient");
	registerService();
	IOLog("PANDORA FAIRPLAY: driver registered (every call answers unsupported)\\n");
	return true;
}

class com_apple_driver_FairPlayIOKitUserClient : public IOUserClient {
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKitUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKitUserClient, IOUserClient);

IOReturn
com_apple_driver_FairPlayIOKitUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
com_apple_driver_FairPlayIOKitUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	static uint32_t logged;
	if (logged < 64) {
		logged++;
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("PANDORA FAIRPLAY: %s sel %u in %u/%u out %u/%u -> unsupported\\n", name, selector,
		    args->scalarInputCount, args->structureInputDescriptor != NULL ?
		    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputDescriptor != NULL ?
		    (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize);
	}
	args->scalarOutputCount = 0;
	return kIOReturnUnsupported;
}
#endif /* PANDORA */
#endif /* SWEET */"""),
])

patch('iokit/KernelConfigTables.cpp', [
    ("""    "     'IOClass'         = AppleKeyStore;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AppleKeyStore;"
    "   },"
""",
     """    "     'IOClass'         = AppleKeyStore;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AppleKeyStore;"
    "   },"
    "   {"
    "     'IOClass'         = com_apple_driver_FairPlayIOKit;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = FairPlayIOKit;"
    "   },"
"""),
])
