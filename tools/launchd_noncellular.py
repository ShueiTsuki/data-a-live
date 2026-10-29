#!/usr/bin/env python3
"""The phone's launchd.plist with the IPSW's non-cellular CommCenter job put back.

  launchd_noncellular.py PHONE_LAUNCHD_PLIST IPSW_LAUNCHD_PLIST OUT

The iOS root came from ios-sweet, whose sweet-root-tweaks.py removed both com.apple.CommCenter jobs (on its device
the cellular one crash-looped without a baseband). iOS ships two jobs of that label: com.apple.CommCenter.plist
loads only where the device tree has a baseband node (_LimitLoadToDeviceTree), com.apple.CommCenter-noncellular.plist
only where it has none (_LimitLoadFromDeviceTree), as on Wi-Fi-only devices. Our device tree (tools/mkadt_pandora.py)
has no baseband node, so the non-cellular job is the one iOS itself would run. Without any CommCenter, vmd
(Visual Voicemail) reconnected to com.apple.commcenter.coretelephony.xpc about 79000 times a boot at 89% of a core
(post12). The phone's file is plistlib's binary output (it reads back byte for byte), so plistlib writes the new one.
PHONE_LAUNCHD_PLIST: tools/ios_cat.py copy of /System/Library/xpc/launchd.plist; IPSW_LAUNCHD_PLIST: the same
file from the IPSW's root. Apple files: OUT stays outside the repo (device/).
"""
import plistlib
import sys

KEY = '/System/Library/LaunchDaemons/com.apple.CommCenter-noncellular.plist'

phone_path, ipsw_path, out = sys.argv[1:4]
phone = plistlib.load(open(phone_path, 'rb'))
ipsw = plistlib.load(open(ipsw_path, 'rb'))
jobs = phone['LaunchDaemons']
if any(job.get('Label') == 'com.apple.CommCenter' for job in jobs.values()):
    sys.exit('the phone already has a com.apple.CommCenter job')
job = ipsw['LaunchDaemons'][KEY]
if job.get('_LimitLoadFromDeviceTree') != 'baseband':
    sys.exit('the IPSW job is not the non-cellular one')
jobs[KEY] = job
data = plistlib.dumps(phone, fmt=plistlib.FMT_BINARY)
if plistlib.loads(data) != phone:
    sys.exit('does not read back')
open(out, 'wb').write(data)
print(f'{out}: {len(jobs)} jobs, {len(data)} bytes')
