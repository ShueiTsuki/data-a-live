#!/bin/bash
# Mesa 85fda3b with UniXNU 1a01c2a's patches (CI order) in /opt/work/lturnip/mesa, made once and kept so ninja
# rebuilds only what changes; then tools/turnip/build_ios_linux.sh into /opt/work/lturnip/out.
set -euo pipefail
. "$HOME/.cargo/env"
REPO=/mnt/d/DeepSeekHarness/claude/ios_pandora
U=/mnt/d/DeepSeekHarness/claude/unixnu_review/src
USHA=1a01c2afa197d975d04da892325c56b6b14aaecb
W=/opt/work/lturnip; mkdir -p $W/abi
if [ ! -f $W/mesa/.pandora-patched ]; then
	rm -rf $W/mesa
	git clone -q --shared --no-checkout /opt/work/mesa_85f $W/mesa
	git -C $W/mesa checkout -q 85fda3b36405bcaea8bffc64aa4f2651dc90770d
	for p in $(git -C $U show $USHA:scripts/data/adreno-compiler-inputs.json | python3 -c 'import json,sys; print(" ".join(json.load(sys.stdin)["patches"]))'); do
		echo "== $p"
		git -C $U show $USHA:$p | git -C $W/mesa apply --whitespace=nowarn -
	done
	touch $W/mesa/.pandora-patched
fi
# Our own edits after UniXNU's, as turnip.yml applies them (each once per tree).
if [ ! -f $W/mesa/.pandora-rpstats ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_rpstats.py $W/mesa
	touch $W/mesa/.pandora-rpstats
fi
if [ ! -f $W/mesa/.pandora-wait ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_wait.py $W/mesa
	touch $W/mesa/.pandora-wait
fi
if [ ! -f $W/mesa/.pandora-dsb ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_dsb.py $W/mesa
	touch $W/mesa/.pandora-dsb
fi
if [ ! -f $W/mesa/.pandora-bocheck ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_bocheck.py $W/mesa
	touch $W/mesa/.pandora-bocheck
fi
if [ ! -f $W/mesa/.pandora-bocheck-submit ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_bocheck_submit.py $W/mesa
	touch $W/mesa/.pandora-bocheck-submit
fi
if [ ! -f $W/mesa/.pandora-bocheck-finish ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_bocheck_finish.py $W/mesa
	touch $W/mesa/.pandora-bocheck-finish
fi
if [ ! -f $W/mesa/.pandora-bo-finish-order ]; then
	python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/turnip/pandora_bo_finish_order.py $W/mesa
	touch $W/mesa/.pandora-bo-finish-order
fi
echo "mesa $(git -C $W/mesa rev-parse --short HEAD), $(git -C $W/mesa status --short | wc -l) files changed by the patches"
tr -d '\r' < $REPO/wsl/adreno/NativeAdrenoABI.h > $W/abi/NativeAdrenoABI.h
cd $REPO
SDK=/opt/work/xcode/sdk16/iPhoneOS16.1.sdk MESON=/opt/work/venv/bin/meson PATH=/opt/work/venv/bin:$PATH \
	bash tools/turnip/build_ios_linux.sh $W/mesa $W/abi $W/out
