def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0310: who counts IOSurface uses ---------------------------------------------------------------------------------
# shot3 (the plugin counting its surface textures as uses): words in Settings still flash as another row's for a moment
# (user: 「一闪就过去了，比之前快很多」). Whether the render server counts the surfaces of its layers itself
# (IOSurfaceIncrementUseCount, traps 0/1) decides where the remaining gap is: the first 40 calls are logged with the
# process and the surface, then each process's totals every 1000 calls.
patch('iokit/Kernel/arm/SweetSurface.cpp', [
    ("""IOReturn
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
}""",
     """/* 0310: who calls traps 0/1. HOW: 0 not this client's surface, 1 counted, 2 a decrement with nothing to drop. */
static void
sweet_use_note(uint32_t id, int delta, int how, uint32_t uses)
{
	static struct {
		char name[20];
		uint32_t inc, dec, missed;
	} who[8];
	static uint32_t calls;
	char name[20];
	proc_selfname(name, sizeof(name));
	IOLockLock(sweet_surface_lock);
	const uint32_t n = ++calls;
	unsigned i = 0;
	while (i < 8 && who[i].name[0] != 0 && strncmp(who[i].name, name, sizeof(name)) != 0) {
		i++;
	}
	if (i == 8) {
		i = 7;                          /* the last row takes everyone else */
	} else if (who[i].name[0] == 0) {
		strlcpy(who[i].name, name, sizeof(who[i].name));
	}
	if (how != 1) {
		who[i].missed++;
	} else if (delta > 0) {
		who[i].inc++;
	} else {
		who[i].dec++;
	}
	if (n <= 40) {
		IOLog("SWEET SURF: use %u: %s surface %u %+d -> %s, its uses %u\\n", n, name, id, delta,
		    how == 0 ? "not its surface" : how == 1 ? "counted" : "nothing to drop", uses);
	}
	if (n % 1000 == 0) {
		for (unsigned k = 0; k < 8 && who[k].name[0] != 0; k++) {
			IOLog("SWEET SURF: uses after %u calls: %s +%u -%u missed %u\\n", n, who[k].name, who[k].inc, who[k].dec,
			    who[k].missed);
		}
	}
	IOLockUnlock(sweet_surface_lock);
}

IOReturn
IOSurfaceRootUserClient::useCount(uint32_t id, int delta)
{
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(id, NULL);
	if (ref == NULL) {
		IOLockUnlock(fLock);
		sweet_use_note(id, delta, 0, 0);
		return kIOReturnNotFound;
	}
	if (delta < 0 && ref->uses == 0) {
		IOLockUnlock(fLock);
		sweet_use_note(id, delta, 2, 0);
		return kIOReturnSuccess;   /* more decrements than increments: nothing of ours to drop */
	}
	ref->uses += delta;
	sweet_surface_use(ref->surface, delta);
	const uint32_t uses = ref->uses;
	IOLockUnlock(fLock);
	sweet_use_note(id, delta, 1, uses);
	return kIOReturnSuccess;
}"""),
])
print('patched (0310 use count log)')
