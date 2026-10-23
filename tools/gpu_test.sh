#!/bin/bash
# One GPU test round on the phone, from Git Bash:
#
#   tools/gpu_test.sh NAME TURNIP_ZIP [--write-pack] [--secs N] [--kernel K] [--level L] [--args "BOOT-ARGS"] [--no-metal]
#
#   1. unpacks TURNIP_ZIP (the turnip-ios artifact of .github/workflows/turnip.yml) into
#      out-root/turnip/NAME, or with TURNIP_ZIP = release downloads the rolling turnip-ci
#      pre-release, and checks its SHA256SUMS
#   2. trust cache for vktest and libvulkan_freedreno.dylib (tools/pgputest/mktc.py)
#   3. the GPU firmware pack (tools/mkfwpack.py): gen80200_{gmu,sqe,zap} from out-root/fwpack_src,
#      vktest, the Turnip library and files.txt installing them (bsd/dev/pandora_files.c)
#   4. with --write-pack only: checks the pinned pack file on the phone and writes the new pack
#      over it raw (tools/pinfile rawwrite), then hashes it back. Without the flag the phone's pack
#      must already be this pack, or the run stops.
#   5. boots iOS (tools/ios_test.sh) with GPUPOOL=1024, pandora_gpu=LEVEL (15) and vktest in
#      place of bluetoothd (sweetexecswap)
#   6. prints the GPU, installer and VKTEST lines of device/logs/NAME.txt
set -euo pipefail
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=/d/Android/platform-tools/adb.exe
NAME=$1 ZIP=$2; shift 2
WRITE=0 SECS=180 KERNEL=/opt/work/plk/k0187-amfi.macho LEVEL=15 ARGS= METAL=1 CLIENTS="bluetoothd vktest mtltest"
while [ $# -gt 0 ]; do
	case $1 in
	--write-pack) WRITE=1; shift ;;
	--secs) SECS=$2; shift 2 ;;
	--kernel) KERNEL=$2; shift 2 ;;
	--level) LEVEL=$2; shift 2 ;;
	--args) ARGS=$2; shift 2 ;;
	--no-metal) METAL=0; shift ;;
	--clients) CLIENTS=$2; shift 2 ;;     # NativeAdrenoMetal.clients: processes the plugin gives a device
	*) echo "unknown option $1"; exit 2 ;;
	esac
done
winp() { echo "$1" | sed -E 's|^/([a-z])/|\1:/|'; }
wslp() { echo "/mnt/$(echo "${1:1:1}" | tr A-Z a-z)${1:2}"; }
T=$ROOT/out-root/turnip/$NAME
PACK=$ROOT/out-root/fwpack-$NAME.img

echo "== 1. Turnip artifact"
rm -rf "$T" && mkdir -p "$T"
if [ "$ZIP" = release ]; then
	# The rolling turnip-ci pre-release (turnip.yml): public, no sign-in needed.
	R=https://github.com/ShueiTsuki/ios-pandora/releases/download/turnip-ci
	for f in libvulkan_freedreno.dylib vktest SHA256SUMS BUILD.txt; do
		curl -sSfL --retry 3 -o "$(winp "$T/$f")" "$R/$f"
	done
	cat "$T/BUILD.txt"
	# The Metal plugin and mtltest, when the build published them (and --no-metal is not given).
	if [ $METAL = 1 ] && curl -sSfL --retry 3 -o "$(winp "$T/SHA256SUMS.metal")" "$R/SHA256SUMS.metal"; then
		for f in NativeAdrenoMetal NativeAdrenoMetal.Info.plist mtltest; do
			curl -sSfL --retry 3 -o "$(winp "$T/$f")" "$R/$f"
		done
		# SHA256SUMS.metal names the bundle paths; the release carries the same bytes under flat names.
		sed -e 's|NativeAdrenoMetal.bundle/NativeAdrenoMetal$|NativeAdrenoMetal|' \
			-e 's|NativeAdrenoMetal.bundle/Info.plist$|NativeAdrenoMetal.Info.plist|' "$T/SHA256SUMS.metal" >> "$T/SHA256SUMS"
	fi
else
	python -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$(winp "$ZIP")" "$(winp "$T")"
fi
(cd "$T" && sha256sum -c SHA256SUMS)
HAVE_METAL=0; [ -f "$T/NativeAdrenoMetal" ] && HAVE_METAL=1

echo "== 2. trust cache"
SIGNED=("$(winp "$T/vktest")" "$(winp "$T/libvulkan_freedreno.dylib")")
[ $HAVE_METAL = 1 ] && SIGNED+=("$(winp "$T/NativeAdrenoMetal")" "$(winp "$T/mtltest")")
python -I "$(winp "$ROOT/tools/pgputest/mktc.py")" "$(winp "$T/gpu.tc")" "${SIGNED[@]}"
wsl -d Ubuntu-24.04 -u root -- bash -c "mkdir -p /opt/work/gputest && cp '$(wslp "$T/gpu.tc")' /opt/work/gputest/$NAME.tc"

echo "== 3. firmware pack"
printf 'vktest 755 /usr/libexec/vktest\nlibvulkan_freedreno.dylib 755 /usr/lib/libvulkan_freedreno.dylib\n' > "$T/files.txt"
F=$ROOT/out-root/fwpack_src
PARTS=("$(winp "$F/gen80200_gmu.bin")" "$(winp "$F/gen80200_sqe.fw")" "$(winp "$F/gen80200_zap.mbn")"
	"$(winp "$T/vktest")" "$(winp "$T/libvulkan_freedreno.dylib")")
if [ $HAVE_METAL = 1 ]; then
	# The bundle directory first (patch 0185), then its files; the plugin hands a device only to the
	# processes named in NativeAdrenoMetal.clients (tools/metal/adapt_ios16.py).
	E=/System/Library/Extensions
	printf '%s\n' "$CLIENTS" > "$T/NativeAdrenoMetal.clients"
	printf -- '- 755 %s\nNativeAdrenoMetal 755 %s\nNativeAdrenoMetal.Info.plist 644 %s\nNativeAdrenoMetal.clients 644 %s\nmtltest 755 /usr/libexec/mtltest\n' \
		"$E/NativeAdrenoMetal.bundle" "$E/NativeAdrenoMetal.bundle/NativeAdrenoMetal" \
		"$E/NativeAdrenoMetal.bundle/Info.plist" "$E/NativeAdrenoMetal.clients" >> "$T/files.txt"
	PARTS+=("$(winp "$T/NativeAdrenoMetal")" "$(winp "$T/NativeAdrenoMetal.Info.plist")"
		"$(winp "$T/NativeAdrenoMetal.clients")" "$(winp "$T/mtltest")")
	case " $CLIENTS " in *" backboardd "*)
		# With a Metal device the render server picks the 10-bit 'b3a8' deep framebuffer (bb2 log), which
		# the display copy takes as BGRA; UniXNU's add-gpu-plugin-data.py turns that and the compression
		# off in the render server's preferences for mobile and root. Only backboardd's own domain here:
		# the installer writes whole files and .GlobalPreferences holds the language and the rest.
		python -I -c "import plistlib, sys; open(sys.argv[1], 'wb').write(plistlib.dumps({'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False, 'CAEnableIOSurfaceCompression': False}, fmt=plistlib.FMT_BINARY))" "$(winp "$T/backboardd.plist")"
		printf 'backboardd.plist 644 /private/var/mobile/Library/Preferences/backboardd.plist\nbackboardd.plist 644 /private/var/root/Library/Preferences/backboardd.plist\n' >> "$T/files.txt"
		PARTS+=("$(winp "$T/backboardd.plist")") ;;
	esac
fi
cat "$T/files.txt"
python -I "$(winp "$ROOT/tools/mkfwpack.py")" "$(winp "$PACK")" "${PARTS[@]}" "$(winp "$T/files.txt")"
SZ=$(stat -c %s "$PACK"); N=$((SZ / 4096)); WANT=$(sha256sum "$PACK" | cut -c1-64)
echo "pack $SZ bytes, $N blocks, sha256 $WANT"
[ "$SZ" -le 41943040 ] || { echo "the pack is larger than the 40 MiB pinned file on the phone"; exit 5; }

echo "== 4. pack on the phone"
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

echo "== 5. boot"
TC="/opt/work/ipsw/Firmware/087-86698-044.dmg.trustcache /opt/work/ipsw/Firmware/087-86593-047.dmg.trustcache+/opt/work/ipsw/Firmware/087-86715-047.dmg.trustcache+/opt/work/gputest/$NAME.tc"
GPUPOOL=1024 KERNEL=$KERNEL TRUSTCACHE=$TC FW=$FILE bash "$ROOT/tools/ios_test.sh" "$NAME" 51 "$SECS" \
	"pandora_gpu=$LEVEL sweetexecswap=bluetoothd:/usr/libexec/vktest $ARGS" 2>&1 | grep -v NodeJS | tail -4

echo "== 6. results"
L=$ROOT/device/logs/$NAME.txt
grep -E "PANDORA GPU: (buffer pool|slice mask|hw_init rest|level 1[0-5] |self-test)|PANDORA GPU UC: (AGX|client|open)|PANDORA FILES|SWEET EXECSWAP|VKTEST:|MTLTEST:|AMFI: .*(vktest|libvulkan)|dyld|NativeAdrenoMetal|panic\(" "$L" | cut -c1-240 | head -120
echo "VKTEST PASS $(grep -c 'VKTEST: PASS' "$L"), FAIL lines $(grep -c 'VKTEST: .*FAIL' "$L"), restarts $(grep -c 'restart type' "$L")"
