#!/bin/bash
# Unattended iOS test run: boot with the console log file and an automatic reset, wait for
# Android to come back, pull and decode the log.
#
#   [FW=/data/local/tmp/adspfw.img] tools/ios_test.sh NAME RUN SECONDS "extra boot-args"
#
# SWEETFB_HZ: display copies per second (default 30).
# FW: also pass a pinned firmware pack (tools/mkfwpack.py) for the ADSP loader (0144).
# Boots /data/local/tmp/iosroot.img (rd=ufsroot0, ufsrw=1 rootrw=1) with the log file
# /data/local/tmp/ioslog.img, debug=0x8 (a panic resets the phone) and
# pandora_reboot_after=SECONDS (patch 0129). The log ends up in device/logs/NAME.txt.
set -euo pipefail
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=/d/Android/platform-tools/adb.exe
NAME=$1 RUN=$2 SECS=$3 EXTRA=${4:-}
BASE="ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=${SWEETFB_HZ:-30} sweetfb_fill=1 sweetnoexec=bluetoothd,fairplayd.H2"
mkdir -p "$ROOT/device/logs"
bash "$ROOT/tools/boot_ios.sh" --file /data/local/tmp/iosroot.img --run "$RUN" --rd ufsroot0 --check hfsx \
	--log /data/local/tmp/ioslog.img --debug 0x8 ${FW:+--fw "$FW"} \
	--cmdline "$BASE pandora_reboot_after=$SECS $EXTRA" 2>&1 | tail -5
start=$(date +%s)
echo "== waiting for Android (up to $((SECS + 600)) s)"
sleep 60
while :; do
	if [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then break; fi
	if [ $(($(date +%s) - start)) -gt $((SECS + 600)) ]; then
		echo "phone did not come back to Android: needs a long press of the power key"; exit 3
	fi
	sleep 10
done
echo "back in Android after $(($(date +%s) - start)) s"
sleep 5
raw="$ROOT/device/logs/$NAME.raw"
"$ADB" exec-out su -c '/data/local/tmp/pinfile rawread /data/local/tmp/ioslog.img $(readlink -f /dev/block/by-name/userdata) 2>/dev/null' > "$raw"
winp() { echo "$1" | sed -E 's|^/([a-z])/|\1:/|'; }
python -I "$(winp "$ROOT/tools/pandora_log.py")" "$(winp "$raw")" "$(winp "$ROOT/device/logs/$NAME.txt")"
# Frame dump (pandora_fbdump=SECS, patch 0159), if the kernel wrote one.
python -I "$(winp "$ROOT/tools/fbdump.py")" "$(winp "$raw")" "$(winp "$ROOT/device/logs/$NAME")"
rm -f "$raw"
