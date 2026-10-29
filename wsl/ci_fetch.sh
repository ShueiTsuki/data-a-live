#!/bin/bash
# ci_fetch.sh RUN_ID NUMBER: lay out an xnu.yml run for local builds (run in Git Bash; needs gh logged in).
#
#   /opt/work/runNUMBER/bt           build tree (xnu-buildtree: build/, fakeroot/, sdk-kernel-platform/, xnu/)
#   /opt/work/runNUMBER/ci/obj       every object of the kernel link (xnu-objects: objects.tgz)
#   /opt/work/runNUMBER/ci/linklibs  libcc_kext / libfirehose_kernel (xnu-objects: linklibs.tgz)
#   /opt/work/runNUMBER/xnu-kernel   the CI kernel itself (kernel.development.pandora, SHA256SUMS)
#   /opt/work/cilog/xnu-ci-NUMBER.full.log   `gh run view --log` (the compile and link commands)
#
# Then: wsl/local_build.sh NUMBER OUT SRC... builds on that run. NUMBER is the run's number (xnu #57).
set -euo pipefail
RUN=${1:?RUN_ID}; N=${2:?NUMBER}
GH=/d/DeepSeekHarness/claude/tools/gh/bin/gh.exe
REPO=ShueiTsuki/data-a-live
D=/d/DeepSeekHarness/claude/ios_pandora/out-root/ci/$N
rm -rf "$D" && mkdir -p "$D"
"$GH" run download "$RUN" -R $REPO -n xnu-buildtree -n xnu-objects -n xnu-kernel -D "$(cygpath -m "$D")"
"$GH" run view "$RUN" -R $REPO --log > "$D/xnu-ci-$N.full.log"
ls -la "$D" "$D"/*
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/out-root/ci/$N
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-24.04 -u root --exec bash -c "set -e
R=/opt/work/run$N; rm -rf \$R && mkdir -p \$R/bt \$R/ci/obj \$R/xnu-kernel /opt/work/cilog
tar xzf $W/xnu-buildtree/buildtree.tgz -C \$R/bt
tar xzf $W/xnu-objects/objects.tgz -C \$R/ci/obj
tar xzf $W/xnu-objects/linklibs.tgz -C \$R/ci
cp -r $W/xnu-kernel/. \$R/xnu-kernel/
(cd \$R/xnu-kernel && sha256sum -c SHA256SUMS)
tr -d '\r' < $W/xnu-ci-$N.full.log > /opt/work/cilog/xnu-ci-$N.full.log
ls \$R \$R/bt \$R/ci; grep -c ' -c ' /opt/work/cilog/xnu-ci-$N.full.log"
