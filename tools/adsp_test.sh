#!/bin/bash
# One audio DSP (ADSP) test round on the phone, from Git Bash:
#
#   tools/adsp_test.sh NAME --tc RUN [--write-pack] [--secs N] [--kernel K] [--args "BOOT-ARGS"] [--hz N]
#
#   1. the firmware pack (tools/mkfwpack.py): gen80200_{gmu,sqe,zap} and the ADSP images (adsp.mdt,
#      adsp.bNN, adsp_dtb.*) from out-root/fwpack_src. No files.txt: the root file system keeps the
#      Turnip and Metal files the gpu_test.sh run RUN installed, and the GPU runs as in that run.
#      Both packs do not fit the 40 MiB pinned file together: the next gpu_test.sh needs --write-pack.
#   2. with --write-pack only: checks the pinned pack file on the phone and writes the new pack over
#      it raw (tools/pinfile rawwrite), then hashes it back (as gpu_test.sh step 4)
#   3. boots iOS (tools/ios_test.sh) as gpu_test.sh does, with RUN's trust cache
#      (/opt/work/gputest/RUN.tc, for the files it installed) and pandora_adsp=2
#   4. prints the PANDORA ADSP and SCM lines of device/logs/NAME.txt, and how far the GPU came
set -euo pipefail
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=/d/Android/platform-tools/adb.exe
NAME=$1; shift
WRITE=0 SECS=180 KERNEL=/opt/work/plk/k0227-amfi.macho ARGS= TCRUN= HZ=60
while [ $# -gt 0 ]; do
	case $1 in
	--tc) TCRUN=$2; shift 2 ;;
	--write-pack) WRITE=1; shift ;;
	--secs) SECS=$2; shift 2 ;;
	--kernel) KERNEL=$2; shift 2 ;;
	--args) ARGS=$2; shift 2 ;;
	--hz) HZ=$2; shift 2 ;;
	*) echo "unknown option $1"; exit 2 ;;
	esac
done
[ -n "$TCRUN" ] || { echo "--tc RUN: the gpu_test.sh run whose files are on the phone"; exit 2; }
wsl -d Ubuntu-24.04 -u root -- test -f /opt/work/gputest/$TCRUN.tc || { echo "no /opt/work/gputest/$TCRUN.tc"; exit 2; }
winp() { echo "$1" | sed -E 's|^/([a-z])/|\1:/|'; }
PACK=$ROOT/out-root/fwpack-$NAME.img

echo "== 1. firmware pack"
F=$ROOT/out-root/fwpack_src
PARTS=()
for f in "$F"/gen80200_gmu.bin "$F"/gen80200_sqe.fw "$F"/gen80200_zap.mbn "$F"/adsp.mdt "$F"/adsp.b[0-9][0-9] \
	"$F"/adsp_dtb.mdt "$F"/adsp_dtb.b[0-9][0-9]; do
	PARTS+=("$(winp "$f")")
done
python -I "$(winp "$ROOT/tools/mkfwpack.py")" "$(winp "$PACK")" "${PARTS[@]}"
SZ=$(stat -c %s "$PACK"); N=$((SZ / 4096)); WANT=$(sha256sum "$PACK" | cut -c1-64)
echo "pack ${#PARTS[@]} files, $SZ bytes, $N blocks, sha256 $WANT"
[ "$SZ" -le 41943040 ] || { echo "the pack is larger than the 40 MiB pinned file on the phone"; exit 5; }

echo "== 2. pack on the phone"
P=/data/local/tmp/pinfile; FILE=/data/local/tmp/adspfw.img
DEV='$(readlink -f /dev/block/by-name/userdata)'
HAVE=$("$ADB" shell su -c "\"$P rawhash $FILE $DEV $SZ\"" 2>/dev/null | tail -1 | cut -c1-64)
echo "phone has $HAVE"
if [ "$HAVE" != "$WANT" ]; then
	[ $WRITE = 1 ] || { echo "the phone's pack differs; rerun with --write-pack to write it"; exit 3; }
	"$ADB" push "$(winp "$PACK")" /data/local/tmp/fwpack-new.img | tail -1
	"$ADB" shell su -c "\"$P rawwrite $FILE $DEV $N --yes < /data/local/tmp/fwpack-new.img && rm /data/local/tmp/fwpack-new.img\"" | tail -2
	HAVE=$("$ADB" shell su -c "\"$P rawhash $FILE $DEV $SZ\"" 2>/dev/null | tail -1 | cut -c1-64)
	[ "$HAVE" = "$WANT" ] || { echo "written pack reads back as $HAVE"; exit 4; }
	echo "pack written and verified"
fi

echo "== 3. boot"
TC="/opt/work/ipsw/Firmware/087-86698-044.dmg.trustcache /opt/work/ipsw/Firmware/087-86593-047.dmg.trustcache+/opt/work/ipsw/Firmware/087-86715-047.dmg.trustcache+/opt/work/gputest/$TCRUN.tc"
SWEETFB_HZ=$HZ GPUPOOL=1024 KERNEL=$KERNEL TRUSTCACHE=$TC FW=$FILE bash "$ROOT/tools/ios_test.sh" "$NAME" 51 "$SECS" \
	"pandora_gpu=15 sweetexecswap=bluetoothd:/usr/libexec/vktest pandora_adsp=2 $ARGS" 2>&1 | grep -v NodeJS |
	tee "$ROOT/device/logs/$NAME-boot.txt" | tail -4

echo "== 4. results"
L=$ROOT/device/logs/$NAME.txt
grep -aE "PANDORA ADSP|PANDORA SCM|PANDORA GPU: (hw_init rest|self-test|.*failed)|panic\(" "$L" | cut -c1-300 | head -150 || true
echo "GPU: $(grep -acE 'PANDORA GPU: .*(failed|NOT ACKED)' "$L" || true) failure lines, VKTEST PASS $(grep -ac 'VKTEST: PASS' "$L" || true) (r2q: 6), MTLTEST lines $(grep -ac 'MTLTEST:' "$L" || true)"
