#!/bin/bash
# PC test of bootmgr/guard/guard.sh, run inside WSL (copies of phone data stay in /tmp):
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/.../bootmgr/test/guard_test.sh
# The partition is the first 1 MiB of out/boot-menu.img (settings page at 0xd000); pinfile is
# a stand-in that prints saved status lines and extents JSON. Each case runs under dash and
# bash: all in place (nothing written), a moved file, a file no longer pinned, a page already
# off, the stock boot (no menu). Only the armed word and the reason may ever change.
set -u
B=$(cd "$(dirname "$0")/.." && pwd)
R=$B/..
T=$(mktemp -d /tmp/bmguard.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail=0

head -c 1048576 "$B/out/boot-menu.img" > "$T/menu.img"
head -c 1048576 "$R/device/stock/boot_b.img" > "$T/stock.img"
mkdir "$T/fix"
while read -r name _sha json; do
	cp "$R/device/extents/$json" "$T/fix/$name.json"
	echo "/data/local/tmp/$name.img: pinned (GC move failures 0), size 1 bytes, blocks 1" > "$T/fix/$name.status"
done < "$B/out/extents.txt"
cat > "$T/pinfile" <<'EOF'
#!/bin/sh
n=$(basename "$2" .img)
case $1 in
status) cat "$FIX/$n.status" ;;
extents) cat "$FIX/$n.json" ;;
esac
EOF
chmod 755 "$T/pinfile"

# run SHELL DEV FIXDIR -> prints the log
run() {
	rm -f "$T/log"
	FIX=$3 "$1" "$B/guard/guard.sh" --dev "$2" --dir /data/local/tmp --pinfile "$T/pinfile" --log "$T/log" --once
	cat "$T/log" 2>/dev/null | cut -c21-
}

# changed BEFORE AFTER -> "offset-offset ..." of differing bytes (decimal, 1-based from cmp)
changed() {
	cmp -l "$1" "$2" | awk '{print $1 - 1}' | tr '\n' ' '
}

expect() {   # name got want
	if [ "$2" = "$3" ]; then echo "ok   $1: $2"; else echo "BAD  $1: got [$2] want [$3]"; fail=1; fi
}

for sh in dash bash; do
	echo "== $sh"
	cp "$T/menu.img" "$T/dev.img"
	expect "$sh in place" "$(run $sh "$T/dev.img" "$T/fix")" "armed, iosroot ioslog adspfw in place"
	expect "$sh in place, bytes" "$(changed "$T/menu.img" "$T/dev.img")" ""

	cp -r "$T/fix" "$T/moved"
	sed -i 's/\]\]}/], [1, 2, 3, 4]]}/' "$T/moved/ioslog.json"
	cp "$T/menu.img" "$T/dev.img"
	got=$(run $sh "$T/dev.img" "$T/moved" | tr '\n' '|')
	expect "$sh moved" "$got" "iOS turned off in $T/dev.img: ioslog moved|turned off: ioslog moved|"
	expect "$sh moved, armed word" "$(od -A n -t x1 -j $((0xd008)) -N 4 "$T/dev.img" | tr -d ' ')" "00000000"
	expect "$sh moved, reason" "$(dd if="$T/dev.img" bs=1 skip=$((0xd078)) count=64 2>/dev/null | tr -d '\0')" "ioslog moved"
	bytes=$(changed "$T/menu.img" "$T/dev.img")
	outside=$(for o in $bytes; do [ "$o" -ge $((0xd008)) ] && [ "$o" -lt $((0xd00c)) ] && continue
		[ "$o" -ge $((0xd078)) ] && [ "$o" -lt $((0xd0b8)) ] && continue; echo "$o"; done | tr '\n' ' ')
	expect "$sh moved, nothing else written" "$outside" ""
	cp "$T/dev.img" "$T/off.img"
	expect "$sh already off" "$(run $sh "$T/dev.img" "$T/moved")" "boot menu present, iOS already off"
	expect "$sh already off, bytes" "$(changed "$T/off.img" "$T/dev.img")" ""
	rm -rf "$T/moved"

	cp -r "$T/fix" "$T/unpinned"
	echo "/data/local/tmp/adspfw.img: pinned (GC move failures 2), size 1 bytes, blocks 1" > "$T/unpinned/adspfw.status"
	cp "$T/menu.img" "$T/dev.img"
	got=$(run $sh "$T/dev.img" "$T/unpinned" | head -1)
	expect "$sh unpinned" "$got" "iOS turned off in $T/dev.img: adspfw not pinned"
	rm -rf "$T/unpinned"

	cp "$T/stock.img" "$T/dev.img"
	expect "$sh stock boot" "$(run $sh "$T/dev.img" "$T/fix")" "no boot menu in $T/dev.img"
	expect "$sh stock boot, bytes" "$(changed "$T/stock.img" "$T/dev.img")" ""
done
exit $fail
