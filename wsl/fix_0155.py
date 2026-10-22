W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# The original AppleImage4 matches IOPlatformDevice "defaults" (IONameMatch); as on an
# iPhone, publish a nub for /defaults. No other driver here matches it.
patch('iokit/Kernel/arm/SweetPlatformExpert.cpp', [
    ("""	return "('chosen', 'defaults', 'cpus', 'options')";""",
     """#if defined(PANDORA)
	/* /defaults gets a nub: the original AppleImage4 (tools/mkprelink.py) matches it. */
	return "('chosen', 'cpus', 'options')";
#else
	return "('chosen', 'defaults', 'cpus', 'options')";
#endif"""),
])
# IOAESAccelerator for AppleImage4's start (see the file).
open('iokit/Kernel/arm/PandoraPMGRNub.cpp', 'w').write(open(W + 'PandoraPMGRNub.cpp').read().replace('\r', ''))
print('patched')
