W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- IOSurface use counts (part of the display scanning out surfaces itself) --------------------------------
# IOSurface.framework (iOS 16.7) answers IOSurfaceIsInUse from the shared state page: IOSurfaceClient + 0x70 is
# the state page address (the lock result is copied to the client at + 0x68, its + 8 is the state page) and the
# surface is in use when the u64 at state + 0x18 is not 0 (dyld cache, IOSurface 0x1a39832d0). Its
# IOSurfaceIncrementUseCount / DecrementUseCount go to traps 0 and 1 (id, category). Apple's kernel keeps that
# count, and a display driver holds one on every surface it shows, so CoreAnimation does not draw into a buffer
# the display still reads (it takes another one). Here the traps were accepted and did nothing; now each
# client's uses are counted on its mapping (dropped with it when the process goes) and the total is kept at
# state + 0x18; sweet_surface_use() is the display's hold.
S = 'iokit/Kernel/arm/SweetSurface.cpp'
edit(S, '''#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */''',
     '''#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define SWEET_SURFACE_STATE_USE         0x18    /* u64 in the state page: IOSurfaceIsInUse = not 0 (0217) */''')
edit(S, '''static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}''', '''static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}

/* 0217: the surface's use count as IOSurfaceIsInUse reads it (clients' traps and kernel holders together). */
void
sweet_surface_use(SweetSurface *s, int delta)
{
	uint64_t *use = (uint64_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_USE);
	if (delta > 0) {
		__atomic_add_fetch(use, (uint64_t)delta, __ATOMIC_RELEASE);
		return;
	}
	uint64_t old = __atomic_load_n(use, __ATOMIC_ACQUIRE), want;
	do {
		want = old >= (uint64_t)-delta ? old + (uint64_t)(int64_t)delta : 0;
	} while (!__atomic_compare_exchange_n(use, &old, want, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}''')
edit(S, '''	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};''', '''	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;
	uint32_t      uses;         /* 0217: this client's IOSurfaceIncrementUseCount minus decrements */

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};''')
edit(S, '''	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		sweet_surface_put(surface);''', '''	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL && uses != 0) {
		sweet_surface_use(surface, -(int)uses);   /* 0217: the client went with uses held */
		uses = 0;
	}
	if (surface != NULL) {
		sweet_surface_put(surface);''')
edit(S, '''IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}''', '''/* 0217: traps 0 and 1, IOSurfaceIncrementUseCount / DecrementUseCount (id, category). */
IOReturn
IOSurfaceRootUserClient::trapUseCount(void *id, __unused void *category, void *, void *, void *, void *)
{
	return useCount((uint32_t)(uintptr_t)id, 1);
}

IOReturn
IOSurfaceRootUserClient::trapUnuseCount(void *id, __unused void *category, void *, void *, void *, void *)
{
	return useCount((uint32_t)(uintptr_t)id, -1);
}

IOReturn
IOSurfaceRootUserClient::useCount(uint32_t id, int delta)
{
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(id, NULL);
	if (ref == NULL) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}
	if (delta < 0 && ref->uses == 0) {
		IOLockUnlock(fLock);
		return kIOReturnSuccess;   /* more decrements than increments: nothing of ours to drop */
	}
	ref->uses += delta;
	sweet_surface_use(ref->surface, delta);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}''')
edit(S, '''	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);''',
     '''	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapUnuseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn useCount(uint32_t id, int delta);''')
edit(S, '''	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },''',
     '''	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnuseCount },''')
edit('iokit/Kernel/arm/SweetSurface.h', '''/* Retained surface for an id, or NULL. */
SweetSurface *sweet_surface_copy(uint32_t id);''', '''/* Retained surface for an id, or NULL. */
SweetSurface *sweet_surface_copy(uint32_t id);

/* 0217: add DELTA to the use count IOSurfaceIsInUse reads (a display holds one on a surface it shows). */
void sweet_surface_use(SweetSurface *s, int delta);''')
print('patched (use counts)')
