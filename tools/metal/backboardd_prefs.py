#!/usr/bin/env python3
"""com.apple.backboardd.plist for a render server that renders through Metal.

  backboardd_prefs.py OUT

backboardd's preference domain is its bundle ID, com.apple.backboardd (the string is in
/usr/libexec/backboardd; a backboardd.plist is never read: bb4 log). With a Metal device CA picks
the two-plane 10-bit 'b3a8' framebuffer; UniXNU's add-gpu-plugin-data.py turns the deep framebuffer
and both compressions off. The installer writes whole files, so the file keeps what the phone's
copy held (out-root/ios-live.img, 2026-10-23: only the digitizer orientation) plus those three keys.
Writing over the existing file keeps its owner (mobile) and mode.
"""
import plistlib
import sys

prefs = {
    'BKDigitizerPersistentServiceProperties': [
        {'props': {'GraphicsOrientation': 1}, 'sender': {'display': '<builtin>', 'page': 13}},
    ],
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}
with open(sys.argv[1], 'wb') as f:
    f.write(plistlib.dumps(prefs, fmt=plistlib.FMT_BINARY))
