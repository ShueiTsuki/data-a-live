#!/bin/bash
# /opt/work/tree = xnu-8796.141.3 prepared exactly like blacktop/darwin-xnu-build's patches()
# (pinned 663906528d1c): its sed edits, our *.sh, then blacktop's + our *.patch in sorted order,
# skipping any that do not apply (as CI does). One git commit per stage.
set -euo pipefail
P=/mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches
D=/opt/work/dxb
T=/opt/work/tree
rm -rf $T /opt/work/pd && mkdir -p /opt/work/pd
cp $D/patches/*.patch /opt/work/pd/
for f in $P/*.patch $P/*.sh; do tr -d '\r' < $f > /opt/work/pd/$(basename $f); done
[ -d /opt/work/xnu-8796 ] || git -c url."https://ghfast.top/https://github.com/".insteadOf=https://github.com/ clone -q --depth 1 --branch xnu-8796.141.3 https://github.com/apple-oss-distributions/xnu.git /opt/work/xnu-8796
cp -r /opt/work/xnu-8796 $T
cd $T
c() { git add -A && git -c user.name=t -c user.email=t@t commit -qm "$1"; }
rm -rf .git && git init -q && c base
FAKEROOT_DIR=/opt/work/fakeroot
sed -i 's|^AVAILABILITY_PL="${SDKROOT}/${DRIVERKITROOT}|AVAILABILITY_PL="${FAKEROOT_DIR}|g' bsd/sys/make_symbol_aliasing.sh
sed -i 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig
sed -i 's|^LDFLAGS_KERNEL_SDK	= -L$(SDKROOT).*|LDFLAGS_KERNEL_SDK	= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel|g' makedefs/MakeInc.def
sed -i 's|^\(INCFLAGS_SDK[[:space:]]*=\) -I$(SDKROOT)/usr/local/include/\(kernel\)\{0,1\}|\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel|' makedefs/MakeInc.def
sed -i 's|export MIG := $(shell $(XCRUN) -sdk $(SDKROOT) -find mig)|export MIG := $(shell find $(FAKEROOT_DIR) -name "mig")|g' makedefs/MakeInc.cmd
sed -i 's|export MIGCOM := $(shell $(XCRUN) -sdk $(SDKROOT) -find migcom)|export MIGCOM := $(shell find $(FAKEROOT_DIR) -name "migcom")|g' makedefs/MakeInc.cmd
c blacktop-sed
export MACHINE_CONFIG=${MACHINE_CONFIG:-SWEET} MACOS_VERSION=13.5
for s in /opt/work/pd/*.sh; do bash $s; done
c staged
skipped=0
for p in $(cd /opt/work/pd && LC_ALL=C ls *.patch); do
  if git apply --check /opt/work/pd/$p 2>/dev/null; then git apply /opt/work/pd/$p
  else echo "SKIPPED $p"; skipped=$((skipped+1)); fi
done
c patched
echo "skipped: $skipped"; git log --oneline | cat
