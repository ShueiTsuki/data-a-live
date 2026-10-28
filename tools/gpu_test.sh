#!/bin/bash
# One GPU test round on the phone, from Git Bash:
#
#   tools/gpu_test.sh NAME TURNIP_ZIP [--write-pack] [--secs N] [--kernel K] [--level L] [--args "BOOT-ARGS"] [--no-metal]
#                     [--clients "NAMES"] [--no-vktest] [--pool MIB] [--hz N] [--amp-fw DIR] [--log-cfg PLIST]
#
#   1. unpacks TURNIP_ZIP (the turnip-ios artifact of .github/workflows/turnip.yml) into
#      out-root/turnip/NAME, or with TURNIP_ZIP = release downloads the rolling turnip-ci
#      pre-release, or with a directory copies an earlier run's files (out-root/turnip/NAME) or a
#      local build, and checks its SHA256SUMS
#   2. trust cache for vktest and libvulkan_freedreno.dylib (tools/pgputest/mktc.py)
#   3. the GPU firmware pack (tools/mkfwpack.py): gen80200_{gmu,sqe,zap} from out-root/fwpack_src,
#      vktest, the Turnip library and files.txt installing them (bsd/dev/pandora_files.c)
#      --amp-fw DIR: also the speaker amplifiers' firmware from DIR (Android's /odm/firmware:
#      cs35l43-dsp1-spk-prot.wmfw, T-/B-cs35l43-dsp1-spk-prot.bin, T-/B-cs35l43-delta-music-spk-prot.bin),
#      read by bsd/dev/pandora_amp.c; copies of phone files, kept outside the repo. spkgraph.bin there too when
#      made (tools/spk_graph.py from the phone's ACDB), for pandora_adsp.c's speaker graph
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
SWAP="sweetexecswap=bluetoothd:/usr/libexec/vktest" POOL=1024
LOG_CFG=()
while [ $# -gt 0 ]; do
	case $1 in
	--write-pack) WRITE=1; shift ;;
	--secs) SECS=$2; shift 2 ;;
	--kernel) KERNEL=$2; shift 2 ;;
	--level) LEVEL=$2; shift 2 ;;
	--args) ARGS=$2; shift 2 ;;
	--no-metal) METAL=0; shift ;;
	--clients) CLIENTS=$2; shift 2 ;;     # NativeAdrenoMetal.clients: processes the plugin gives a device
	--pool) POOL=$2; shift 2 ;;            # MiB carved for the GPU (0: none)
	--hz) HZ=$2; shift 2 ;;                # display copies per second (sweetfb_hz; ios_test.sh default 30)
	--no-vktest) SWAP=; shift ;;          # vktest not run (launchd restarts it all boot, mtltest with it)
	--metal-from) METAL_FROM=$2; shift 2 ;;  # a local plugin build (tools/metal/build_plugin_linux.sh OUT, WSL path)
	--amp-fw) AMP_FW=$2; shift 2 ;;        # the amplifiers' firmware files (Windows-side directory)
	--log-cfg) LOG_CFG+=("$2"); shift 2 ;; # a unified-log subsystem config (tools/logging/SUBSYSTEM.plist), repeatable
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
	R=https://github.com/ShueiTsuki/data-a-live/releases/download/turnip-ci
	# Large files: their xz copy when the build published one (turnip.yml; downloads here are slow),
	# else the file itself; SHA256SUMS checks what comes out either way.
	fetch() {
		# Resumed after a dropped connection (ten tries) before falling back to the whole file.
		local ok=1 t
		for t in 1 2 3 4 5 6 7 8 9 10; do
			curl -sSfL -C - --max-time 600 -o "$(winp "$T/$1.xz")" "$R/$1.xz" && { ok=0; break; }
		done
		if [ $ok = 0 ]; then
			python -I -c "import lzma,sys; open(sys.argv[1],'wb').write(lzma.decompress(open(sys.argv[1]+'.xz','rb').read()))" "$(winp "$T/$1")"
			rm -f "$T/$1.xz"
		else
			curl -sSfL --retry 3 --max-time 600 -o "$(winp "$T/$1")" "$R/$1"
		fi
	}
	fetch libvulkan_freedreno.dylib
	for f in vktest SHA256SUMS BUILD.txt; do
		curl -sSfL --retry 3 --max-time 600 -o "$(winp "$T/$f")" "$R/$f"
	done
	cat "$T/BUILD.txt"
	# The Metal plugin and mtltest, when the build published them (and --no-metal is not given).
	if [ $METAL = 1 ] && curl -sSfL --retry 3 --max-time 600 -o "$(winp "$T/SHA256SUMS.metal")" "$R/SHA256SUMS.metal"; then
		# With --metal-from the plugin comes from the local build: only mtltest is fetched.
		[ -z "${METAL_FROM:-}" ] && fetch NativeAdrenoMetal
		for f in $([ -z "${METAL_FROM:-}" ] && echo NativeAdrenoMetal.Info.plist) mtltest; do
			curl -sSfL --retry 3 --max-time 600 -o "$(winp "$T/$f")" "$R/$f"
		done
		# SHA256SUMS.metal names the bundle paths; the release carries the same bytes under flat names.
		sed -e 's|NativeAdrenoMetal.bundle/NativeAdrenoMetal$|NativeAdrenoMetal|' \
			-e 's|NativeAdrenoMetal.bundle/Info.plist$|NativeAdrenoMetal.Info.plist|' "$T/SHA256SUMS.metal" >> "$T/SHA256SUMS"
	fi
elif [ -d "$ZIP" ]; then
	# A directory: an earlier run's files (out-root/turnip/NAME) or a local build, nothing downloaded.
	for f in libvulkan_freedreno.dylib vktest SHA256SUMS BUILD.txt NativeAdrenoMetal NativeAdrenoMetal.Info.plist mtltest; do
		[ -f "$ZIP/$f" ] && cp "$ZIP/$f" "$T/"
	done
else
	python -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$(winp "$ZIP")" "$(winp "$T")"
fi
(cd "$T" && sha256sum -c ${METAL_FROM:+--ignore-missing} SHA256SUMS)
if [ -n "${METAL_FROM:-}" ]; then
	# The local build's plugin (and mtltest, when it built one) over the artifact's, checked against its own sums.
	wsl -d Ubuntu-24.04 -u root -- bash -c "cd $METAL_FROM && sha256sum -c SHA256SUMS >&2 &&
		cp NativeAdrenoMetal.bundle/NativeAdrenoMetal $(wslp "$T")/NativeAdrenoMetal &&
		cp NativeAdrenoMetal.bundle/Info.plist $(wslp "$T")/NativeAdrenoMetal.Info.plist &&
		if [ -f mtltest ]; then cp mtltest $(wslp "$T")/mtltest; fi" || exit 1
	echo "plugin from $METAL_FROM: $(sha256sum "$T/NativeAdrenoMetal" | cut -c1-16)"
fi
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
		# With a Metal device the render server picks the two-plane 10-bit 'b3a8' deep framebuffer (bb2
		# log); UniXNU's add-gpu-plugin-data.py turns that and the compressions off in its preferences.
		# Its domain is com.apple.backboardd (bb4: a backboardd.plist is not read); the file keeps what
		# the phone's held (tools/metal/backboardd_prefs.py). .GlobalPreferences is left alone.
		python -I "$(winp "$ROOT/tools/metal/backboardd_prefs.py")" "$(winp "$T/com.apple.backboardd.plist")"
		printf 'com.apple.backboardd.plist 600 /private/var/mobile/Library/Preferences/com.apple.backboardd.plist\n' >> "$T/files.txt"
		PARTS+=("$(winp "$T/com.apple.backboardd.plist")") ;;
	esac
fi
if [ -n "${AMP_FW:-}" ]; then
	for f in cs35l43-dsp1-spk-prot.wmfw T-cs35l43-dsp1-spk-prot.bin B-cs35l43-dsp1-spk-prot.bin 		T-cs35l43-delta-music-spk-prot.bin B-cs35l43-delta-music-spk-prot.bin; do
		[ -f "$AMP_FW/$f" ] || { echo "no $AMP_FW/$f"; exit 1; }
		PARTS+=("$(winp "$AMP_FW/$f")")
	done
	# the speaker graph from the phone's ACDB (tools/spk_graph.py), when made
	if [ -f "$AMP_FW/spkgraph.bin" ]; then
		PARTS+=("$(winp "$AMP_FW/spkgraph.bin")")
	fi
	# this phone's speaker calibration (Android /mnt/vendor/persist/audio/crus_calr.bin), when copied there;
	# goes only into the pack on the phone, never into the repo
	if [ -f "$AMP_FW/crus_calr.bin" ]; then
		PARTS+=("$(winp "$AMP_FW/crus_calr.bin")")
	fi
fi
# --log-cfg: logd's per-subsystem overrides (/Library/Preferences/Logging/Subsystems, next to the system's own in
# /System/Library/Preferences/Logging/Subsystems), e.g. debug level persisted for VirtualAudio's categories
for c in "${LOG_CFG[@]}"; do
	[ -f "$c" ] || { echo "no $c"; exit 1; }
	printf '%s 644 /Library/Preferences/Logging/Subsystems/%s\n' "$(basename "$c")" "$(basename "$c")" >> "$T/files.txt"
	PARTS+=("$(winp "$c")")
done
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
# The whole boot step goes to device/logs/NAME-boot.txt (r2b: the last 4 lines hid what fastboot did).
SWEETFB_HZ=${HZ:-30} GPUPOOL=${POOL#0} KERNEL=$KERNEL TRUSTCACHE=$TC FW=$FILE bash "$ROOT/tools/ios_test.sh" "$NAME" 51 "$SECS" \
	"pandora_gpu=$LEVEL $SWAP $ARGS" 2>&1 | grep -v NodeJS | tee "$ROOT/device/logs/$NAME-boot.txt" | tail -4

echo "== 6. results"
L=$ROOT/device/logs/$NAME.txt
grep -E "PANDORA GPU: (buffer pool|slice mask|hw_init rest|level 1[0-5] |self-test)|PANDORA GPU UC: (AGX|client|open)|PANDORA FILES|SWEET EXECSWAP|VKTEST:|MTLTEST:|AMFI: .*(vktest|libvulkan)|dyld|NativeAdrenoMetal|panic\(" "$L" | cut -c1-240 | head -120 || true
echo "VKTEST PASS $(grep -c 'VKTEST: PASS' "$L"), FAIL lines $(grep -c 'VKTEST: .*FAIL' "$L"), restarts $(grep -c 'restart type' "$L")"
