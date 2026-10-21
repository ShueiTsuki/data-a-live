#!/bin/bash
# Read-only backup of the phone's partitions (everything except userdata and super).
# Each image is streamed with root `cat`, then its SHA-256 is compared with one computed on the phone.
# Usage: tools/backup.sh   (run from the ios_pandora directory)
set -u
export MSYS_NO_PATHCONV=1
A=/d/Android/platform-tools/adb.exe
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/backup"
LIST="$ROOT/device/partitions.txt"
mkdir -p "$OUT"
MANIFEST="$OUT/manifest.txt"
: > "$MANIFEST"

targets=()
for lun in sdb sdc sdd sdf sde; do targets+=("$lun /dev/block/$lun"); done
while read -r name dev size; do
	[[ $dev =~ ^sda[0-9]+$ ]] || continue
	[[ $name == userdata || $name == super ]] && continue
	targets+=("$name /dev/block/by-name/$name")
done < "$LIST"

fail=0
for t in "${targets[@]}"; do
	name=${t%% *}; src=${t#* }
	dst="$OUT/$name.img"
	case "$dst" in "$OUT"/*) ;; *) echo "refusing path $dst"; exit 1;; esac
	for try in 1 2; do
		"$A" exec-out "su -c 'cat $src'" > "$dst"
		pc=$(sha256sum "$dst" | cut -d' ' -f1)
		ph=$("$A" shell "su -c 'sha256sum $src'" | tr -d '\r' | cut -d' ' -f1)
		[[ $pc == "$ph" ]] && break
	done
	bytes=$(stat -c %s "$dst")
	if [[ $pc == "$ph" ]]; then st=ok; else st=MISMATCH; fail=$((fail+1)); fi
	echo "$st $name $bytes $pc" | tee -a "$MANIFEST"
done
echo "done, mismatches: $fail"
