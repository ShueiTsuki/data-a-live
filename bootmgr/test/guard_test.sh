#!/bin/bash
# PC test of bootmgr/guard/guard.sh, run inside WSL (copies of phone data stay in /tmp):
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/.../bootmgr/test/guard_test.sh
# The partition is out/boot-menu.img up to the end of the slot header page (build.sh with
# --slot-from: the slot was made from the newest saved extents in device/extents); pinfile is
# a stand-in that prints saved status lines and extents JSON. Each case runs under dash and
# bash: all in place (nothing written), a moved file, a file no longer pinned, a slot already
# off, an empty slot, the stock boot (no menu). Only the armed word and the reason may change.
set -u
B=$(cd "$(dirname "$0")/.." && pwd)
R=$B/..
T=$(mktemp -d /tmp/bmguard.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail=0

OFF=$(grep -o 'iOS slot at partition offset 0x[0-9a-f]*' "$B/out/layout.txt" | grep -o '0x[0-9a-f]*')
OFF=$((OFF))
head -c $((OFF + 8192)) "$B/out/boot-menu.img" > "$T/menu.img"
head -c $((OFF + 8192)) "$R/device/stock/boot_b.img" > "$T/stock.img"
mkdir "$T/fix"
for name in iosroot ioslog adspfw; do
	cp "$(ls "$R"/device/extents/$name-*.json | sort | tail -1)" "$T/fix/$name.json"
	echo "/data/local/tmp/$name.img: pinned (GC move failures 0), size 1 bytes, blocks 1" > "$T/fix/$name.status"
done
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

# offsets (0-based) of the bytes that differ
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
	expect "$sh moved, armed word" "$(od -A n -t x1 -j $((OFF + 8)) -N 4 "$T/dev.img" | tr -d ' ')" "00000000"
	expect "$sh moved, reason" "$(dd if="$T/dev.img" bs=1 skip=$((OFF + 128)) count=64 2>/dev/null | tr -d '\0')" "ioslog moved"
	bytes=$(changed "$T/menu.img" "$T/dev.img")
	outside=$(for o in $bytes; do [ "$o" -ge $((OFF + 8)) ] && [ "$o" -lt $((OFF + 12)) ] && continue
		[ "$o" -ge $((OFF + 128)) ] && [ "$o" -lt $((OFF + 192)) ] && continue; echo "$o"; done | tr '\n' ' ')
	expect "$sh moved, nothing else written" "$outside" ""
	cp "$T/dev.img" "$T/off.img"
	expect "$sh already off" "$(run $sh "$T/dev.img" "$T/moved")" "iOS already off"
	expect "$sh already off, bytes" "$(changed "$T/off.img" "$T/dev.img")" ""
	rm -rf "$T/moved"

	cp -r "$T/fix" "$T/unpinned"
	echo "/data/local/tmp/adspfw.img: pinned (GC move failures 2), size 1 bytes, blocks 1" > "$T/unpinned/adspfw.status"
	cp "$T/menu.img" "$T/dev.img"
	got=$(run $sh "$T/dev.img" "$T/unpinned" | head -1)
	expect "$sh unpinned" "$got" "iOS turned off in $T/dev.img: adspfw not pinned"
	rm -rf "$T/unpinned"

	cp "$T/menu.img" "$T/dev.img"
	dd if=/dev/zero of="$T/dev.img" bs=1 seek=$OFF count=4096 conv=notrunc 2>/dev/null
	cp "$T/dev.img" "$T/empty.img"
	expect "$sh empty slot" "$(run $sh "$T/dev.img" "$T/fix")" "iOS slot empty"
	expect "$sh empty slot, bytes" "$(changed "$T/empty.img" "$T/dev.img")" ""

	cp "$T/stock.img" "$T/dev.img"
	expect "$sh stock boot" "$(run $sh "$T/dev.img" "$T/fix")" "no boot menu in $T/dev.img"
	expect "$sh stock boot, bytes" "$(changed "$T/stock.img" "$T/dev.img")" ""
done
exit $fail
