#!/bin/bash
# Put an iOS build into the boot menu's iOS slot on the phone (Git Bash; phone in Android
# with root). The menu and the Android kernel in boot_<slot> are not touched.
#
#   bootmgr/update_ios.sh [--loader FILE] [--keep-args] [--yes]
#
# --loader     what `fastboot boot` starts (default shim/loader/out/loader-boot.img)
# --keep-args  keep the test-only boot-args (pandora_reboot_after, pandora_fbdump)
# --yes        write it; without it everything is checked and nothing is written
#
# 1. reads the slot's place from boot_<slot> and the three pinned files' extents now
# 2. slot.py: the build's ADT must hold exactly these extents, else refused
# 3. pushes the slot and writes it (phone/update_helper.sh: header cleared first, written
#    last; read back; everything outside the slot must hash as before)
set -euo pipefail
B="$(cd "$(dirname "$0")" && pwd)"
PT=/d/Android/platform-tools
ADB="$PT/adb.exe"
loader="$B/../shim/loader/out/loader-boot.img"
keep=() yes=
while [ $# -gt 0 ]; do
	case $1 in
	--loader) loader=$2; shift 2 ;;
	--keep-args) keep=(--keep-args); shift ;;
	--yes) yes=1; shift ;;
	*) sed -n '2,16p' "$0"; exit 2 ;;
	esac
done
su() { "$ADB" shell su -c "'$*'" | tr -d '\r'; }
[ "$("$ADB" shell getprop ro.product.device | tr -d '\r')" = pandora ] || { echo "no pandora on adb"; exit 1; }
# Copies of phone data stay outside the repo.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
H=/data/local/tmp/bm_update_helper.sh
"$ADB" push "$(cygpath -w "$B/phone/update_helper.sh")" $H > /dev/null

echo "== phone"
info=$(su "sh $H info")
echo "$info"
case $info in *update_helper:*) exit 1 ;; esac
max=$(echo "$info" | awk '/^slot /{print $3}')
n=$(echo "$info" | grep -c ': pinned (GC move failures 0)' || true)
[ "$n" = 3 ] || { echo "refused: the three files must be pinned with no GC move failures"; exit 1; }
for f in iosroot ioslog adspfw; do
	su "/data/local/tmp/pinfile extents /data/local/tmp/$f.img --json" > "$T/$f.json"
done

echo "== slot from $(basename "$loader") ($(date -r "$loader" '+%m-%d %H:%M'))"
python -I "$B/slot.py" "$loader" "$T/slot.img" "${keep[@]}" \
	--label "$(basename "$loader") $(date -r "$loader" +%Y%m%d-%H%M%S)" \
	--extents "$T/iosroot.json" "$T/ioslog.json" "$T/adspfw.json"
size=$(stat -c %s "$T/slot.img")
[ "$size" -le "$max" ] || { echo "refused: slot $size bytes, room for $max (compress the Image)"; exit 1; }
echo "fits: $size of $max bytes"
if [ -z "$yes" ]; then
	echo "checked only; --yes writes it"
	exit 0
fi

echo "== write"
"$ADB" push "$(cygpath -w "$T/slot.img")" /data/local/tmp/bm_slot.img > /dev/null
local_sum=$(sha256sum < "$T/slot.img" | cut -c1-64)
phone_sum=$(su "sha256sum /data/local/tmp/bm_slot.img" | cut -c1-64)
[ "$local_sum" = "$phone_sum" ] || { echo "push corrupted ($phone_sum)"; su "rm /data/local/tmp/bm_slot.img"; exit 1; }
result=$(su "sh $H write /data/local/tmp/bm_slot.img")
su "rm -f /data/local/tmp/bm_slot.img"
echo "$result"
case $result in "write ok"*) ;; *) exit 1 ;; esac
