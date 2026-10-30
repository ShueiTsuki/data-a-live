#!/bin/bash
# replay.sh <spec> [out]  -- runs the harness on lavapipe
. $HOME/.cargo/env
export PATH=$(rustup run 1.98.0 rustc --print sysroot)/lib/rustlib/x86_64-unknown-linux-gnu/bin:$PATH
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export XDG_CACHE_HOME=/opt/work/replay/xdgcache
exec /opt/work/replay/target/release/m2v-replay "$@"
