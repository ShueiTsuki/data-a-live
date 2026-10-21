#!/bin/bash
# Back up one directory of the phone's /data, one tar per top-level entry, read-only on the phone.
#   backup_userdata.sh TAG PHONE_DIR OUT_DIR
# Resumable: an entry whose tar already passed verification is skipped.
# Verification per entry: regular-file count and total bytes from the phone (find/stat)
# against the tar's own listing on the PC, plus tar's error output on the phone.
set -u
export MSYS_NO_PATHCONV=1
A=/d/Android/platform-tools/adb.exe
TAG=$1 PDIR=$2 OUT=$3
HELPER=/data/local/tmp/bk.sh
mkdir -p "$OUT"
LOG="$OUT/verify.log"

"$A" push "$(dirname "$0")/bk.sh" $HELPER >/dev/null || exit 1
"$A" shell "su -c 'sh $HELPER list $TAG $PDIR'" | tr -d '\r' > "$OUT/manifest.tsv"
n=$(wc -l < "$OUT/manifest.tsv")
echo "$(date '+%F %T') $TAG: $n entries in $PDIR" | tee -a "$LOG"

fail=0
while IFS=$'\t' read -r idx name; do
	tarf="$OUT/$(printf %04d "$idx").tar"
	case "$tarf" in "$OUT"/*) ;; *) echo "bad path $tarf"; exit 1;; esac
	[ -f "$tarf.ok" ] && continue
	read -r pf pb < <("$A" shell "su -c 'sh $HELPER stat $TAG $idx'" < /dev/null | tr -d '\r')
	t0=$(date +%s)
	"$A" exec-out "su -c 'sh $HELPER tar $TAG $idx'" < /dev/null > "$tarf"
	t1=$(date +%s)
	err=$("$A" shell "su -c 'cat /data/local/tmp/bk_${TAG}_$idx.err'" < /dev/null | tr -d '\r')
	read -r tf tb < <(tar -tvf "$tarf" 2>/dev/null | awk '$1 ~ /^-/ { n++; s += $3 } END { printf "%d %d\n", n, s }')
	size=$(stat -c %s "$tarf")
	secs=$((t1 - t0)); [ $secs -gt 0 ] || secs=1
	if [ "$pf" = "$tf" ] && [ "$pb" = "$tb" ] && [ -z "$err" ]; then
		st=ok; : > "$tarf.ok"
	else
		st=CHECK; fail=$((fail + 1))
	fi
	printf '%s %s %04d phone %s files %s bytes | tar %s files %s bytes | %s MB in %ss (%s MB/s) | %s | err: %s\n' \
		"$(date '+%T')" "$st" "$idx" "$pf" "$pb" "$tf" "$tb" $((size >> 20)) $secs $(( (size >> 20) / secs )) "$name" "${err:-none}" | tee -a "$LOG"
done < "$OUT/manifest.tsv"
echo "$(date '+%F %T') $TAG done, entries needing a look: $fail" | tee -a "$LOG"
