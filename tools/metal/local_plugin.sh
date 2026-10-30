#!/bin/bash
# The Metal plugin from a clean NativeMetal c6d29ca (adapt_ios16.py patches it in place) into /opt/work/lplugin/out.
set -euo pipefail
. "$HOME/.cargo/env"
W=/opt/work/lplugin; S=$W/src
git -C $S/nativemetal checkout -q -- . && git -C $S/nativemetal clean -fdq
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
SDK=/opt/work/xcode/sdk16/iPhoneOS16.1.sdk bash tools/metal/build_plugin_linux.sh \
	$S/unixnu $S/nativemetal $S/nativeadreno $W/metal2vulkan /opt/work/mesa_85f $W/out
