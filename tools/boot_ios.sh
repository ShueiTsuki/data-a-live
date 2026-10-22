#!/bin/bash
# One command for a PANDORA boot with a root (or test) file pinned in Android's userdata.
# Runs in Git Bash on Windows (adb/fastboot are Windows binaries); payload work in WSL.
#
#   tools/boot_ios.sh --file /data/local/tmp/iostest.img --run 19 [--cmdline "..."]
#                     [--cpus 8] [--rd md0|ufsroot0] [--check pattern|hfsx|none]
#                     [--log /data/local/tmp/ioslog.img] [--fw /data/local/tmp/adspfw.img]
#                     [--debug 0x8] [--dry-run]
#    (--rd ufsroot0: root on the pinned file itself, no RAM disk;
#     --log: a second pinned file for the kernel console log, patch 0129, needs ufsrw=1;
#     --fw: a pinned firmware pack (tools/mkfwpack.py) for patch 0144's ADSP loader;
#     --debug 0x8: panics reset the phone instead of waiting, default 0xc)
#
# 1. Phone in Android (adb): each file must be pinned with no GC move failures; its extent
#    list is saved to device/extents/<name>-<time>.json and must equal the last saved one
#    (a change means f2fs moved it: stop and look). --check pattern runs pinfile rawcheck
#    (test file), --check hfsx reads its first block raw and wants the HFSX signature.
# 2. WSL: ADT with those extents (mkadt_pandora.py --root-extents/--log-extents) + payload
#    from /opt/work/runN, then shim/loader/build.sh -> shim/loader/out/loader-boot.img.
# 3. adb reboot bootloader, check the product is pandora, fastboot boot (RAM only).
# Nothing here writes to the phone's storage.
set -euo pipefail
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PT=/d/Android/platform-tools
ADB="$PT/adb.exe" FB="$PT/fastboot.exe"
FILE="" RUN="" CMDLINE="" CPUS=8 CHECK=none DRY=0 RD=md0 LOG="" FW="" DEBUG=0xc
while [ $# -gt 0 ]; do
	case $1 in
	--file) FILE=$2; shift 2 ;;
	--rd) RD=$2; shift 2 ;;
	--run) RUN=$2; shift 2 ;;
	--cmdline) CMDLINE=$2; shift 2 ;;
	--cpus) CPUS=$2; shift 2 ;;
	--check) CHECK=$2; shift 2 ;;
	--log) LOG=$2; shift 2 ;;
	--fw) FW=$2; shift 2 ;;
	--debug) DEBUG=$2; shift 2 ;;
	--dry-run) DRY=1; shift ;;
	*) sed -n '2,21p' "$0"; exit 2 ;;
	esac
done
[ -n "$FILE" ] && [ -n "$RUN" ] || { sed -n '2,21p' "$0"; exit 2; }
P=/data/local/tmp/pinfile
su() { "$ADB" shell su -c "'$*'" | tr -d '\r'; }

# pinned_extents FILE -> prints the saved JSON path (or exits)
pinned_extents() {
	local f=$1 st name last now
	st=$(su "$P status $f"); echo "$st" >&2
	case $st in *": pinned (GC move failures 0)"*) ;; *) echo "refused: $f not pinned or GC tried to move it" >&2; exit 1 ;; esac
	mkdir -p "$ROOT/device/extents"
	name=$(basename "$f" .img)
	last=$(ls -t "$ROOT/device/extents/$name"-*.json 2>/dev/null | head -1 || true)
	now="$ROOT/device/extents/$name-$(date +%Y%m%d-%H%M%S).json"
	su "$P extents $f --json" > "$now"
	if [ -n "$last" ] && ! cmp -s "$last" "$now"; then
		echo "refused: extents of $f differ from $last (f2fs moved the file?)" >&2; exit 1
	fi
	echo "extents: $(basename "$now")${last:+ (same as $(basename "$last"))}" >&2
	echo "$now"
}
wslpath_of() { echo "/mnt/$(echo "${1:1:1}" | tr A-Z a-z)${1:2}"; }

echo "== 1. phone (Android)"
[ "$("$ADB" shell getprop ro.product.device | tr -d '\r')" = pandora ] || { echo "no pandora on adb"; exit 1; }
now=$(pinned_extents "$FILE")
case $CHECK in
pattern) su "$P rawcheck $FILE" | tail -1 | grep -q ", 0 do not" || { echo "refused: test pattern incomplete"; exit 1; } ;;
hfsx)
	off=$(python -I -c "import json,sys; print(json.load(open(sys.argv[1]))['extents'][0][1])" "$(cygpath -w "$now")")
	sig=$(su "dd if=/dev/block/by-name/userdata bs=4096 skip=$((off / 4096)) count=1 2>/dev/null | dd bs=1 skip=1024 count=2 2>/dev/null")
	[ "$sig" = HX ] || { echo "refused: no HFSX signature at the root's first block (got '$sig')"; exit 1; } ;;
none) ;;
*) echo "--check pattern|hfsx|none"; exit 2 ;;
esac
ADTFLAGS="--root-extents $(wslpath_of "$now")"
if [ -n "$LOG" ]; then
	lognow=$(pinned_extents "$LOG")
	ADTFLAGS="$ADTFLAGS --log-extents $(wslpath_of "$lognow")"
fi
if [ -n "$FW" ]; then
	fwnow=$(pinned_extents "$FW")
	ADTFLAGS="$ADTFLAGS --fw-extents $(wslpath_of "$fwnow")"
fi

echo "== 2. payload (WSL, /opt/work/run$RUN)"
# KERNEL=/opt/work/plk/kN-amfi.macho (tools/mkprelink.py) and TRUSTCACHE="IPSW trust cache..."
# pass through to make_phone_payload.sh.
wsl -d Ubuntu-24.04 -u root -- bash -c "KERNEL='${KERNEL:-}' TRUSTCACHE='${TRUSTCACHE:-}' ADTFLAGS='$ADTFLAGS' CPUS=$CPUS RD=$RD DEBUG=$DEBUG NAME=out \
	bash <(tr -d '\r' < /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/make_phone_payload.sh) /opt/work/run$RUN '$CMDLINE'" 2>&1 |
	grep -v NodeJS | tail -3
bash "$ROOT/shim/loader/build.sh" | tail -1
strings_cmd=$(grep -a -o "debug=0x[0-9a-f]* serial=3 cpus=[0-9] [ -~]*" "$ROOT/shim/loader/out/payload.bin" | head -1)
echo "cmdline: $strings_cmd"
[ $DRY = 1 ] && { echo "dry run: not booting"; exit 0; }

echo "== 3. fastboot boot"
"$ADB" reboot bootloader
for _ in $(seq 1 30); do sleep 2; "$FB" devices | grep -q fastboot && break; done
[ "$("$FB" getvar product 2>&1 | sed -n 's/^product: //p' | tr -d '\r')" = pandora ] || { echo "fastboot device is not pandora"; exit 1; }
"$FB" boot "$(cygpath -w "$ROOT/shim/loader/out/loader-boot.img")"
