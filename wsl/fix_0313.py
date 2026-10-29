W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0313: every thread's user call stack of named processes ---------------------------------------------------------
# The Astronomy poster's editor (AegirPoster) waits on its main thread for locationd's answer to
# getAuthorizationStatusForBundleID (CLLocationInternalClient, the SPI service) and is killed after 5 s ("extension
# check-in timeout"). locationd takes that service's connections only in its first second after boot (post1, post2),
# then none, so one of its queues hangs. Boot-arg pandora_stacks=locationd (pandora_stacks_at, pandora_stacks_every)
# logs where each of its threads waits (pandora_prof.c).
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
