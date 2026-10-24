#!/bin/bash
# PC test of bootmgr/phone/update_helper.sh, run inside WSL under dash (copies stay in /tmp):
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/.../bootmgr/test/update_test.sh
# The partition is a copy of out/boot-menu.img. A second slot (out/slot.img with a changed
# label and checksum-consistent body) is written into it: afterwards the slot holds exactly
# the new file and every byte outside the slot is unchanged. A slot too large and a file that
# is not a slot are refused without writing.
set -u
B=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d /tmp/bmupdate.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail=0
expect() {   # name got want
	if [ "$2" = "$3" ]; then echo "ok   $1: $2"; else echo "BAD  $1: got [$2] want [$3]"; fail=1; fi
}

cp "$B/out/boot-menu.img" "$T/part.img"
cp "$T/part.img" "$T/orig.img"
OFF=$(($(grep -o 'iOS slot at partition offset 0x[0-9a-f]*' "$B/out/layout.txt" | grep -o '0x[0-9a-f]*')))
MAX=$(grep -o 'offset 0x[0-9a-f]*, [0-9]* bytes' "$B/out/layout.txt" | awk '{print $3}')
# a different slot: the same Image shortened by one page, with a new label (the menu would
# call its checksum wrong, which does not matter here)
size=$(stat -c %s "$B/out/slot.img")
head -c $((size - 4096)) "$B/out/slot.img" > "$T/new.img"
printf 'update test slot' | dd of="$T/new.img" bs=1 seek=192 conv=notrunc 2>/dev/null
H="$B/phone/update_helper.sh"

info=$(BM_DEV="$T/part.img" dash "$H" info 2>&1 | head -2 | tr '\n' '|')
expect "info" "$info" "dev $T/part.img|slot $OFF $MAX|"

out=$(BM_DEV="$T/part.img" dash "$H" write "$T/new.img" 2>&1)
expect "write" "$out" "write ok: $((size - 4096)) bytes at $OFF in $T/part.img, outside unchanged"
expect "slot holds the new file" "$(dd if="$T/part.img" bs=1 skip=$OFF count=$((size - 4096)) 2>/dev/null | sha256sum | cut -c1-64)" \
	"$(sha256sum < "$T/new.img" | cut -c1-64)"
expect "before the slot unchanged" "$(head -c $OFF "$T/part.img" | sha256sum | cut -c1-16)" "$(head -c $OFF "$T/orig.img" | sha256sum | cut -c1-16)"
expect "after the slot unchanged" "$(tail -c +$((OFF + MAX + 1)) "$T/part.img" | sha256sum | cut -c1-16)" \
	"$(tail -c +$((OFF + MAX + 1)) "$T/orig.img" | sha256sum | cut -c1-16)"

cp "$T/part.img" "$T/before.img"
head -c $((MAX + 4096)) /dev/zero > "$T/big.img"
printf 'PNDSLOT1' | dd of="$T/big.img" conv=notrunc 2>/dev/null
out=$(BM_DEV="$T/part.img" dash "$H" write "$T/big.img" 2>&1)
expect "too large refused" "$out" "update_helper: slot file $((MAX + 4096)) bytes, room for $MAX"
head -c 8192 /dev/urandom > "$T/junk.img"
out=$(BM_DEV="$T/part.img" dash "$H" write "$T/junk.img" 2>&1)
expect "not a slot refused" "$out" "update_helper: $T/junk.img is not a slot"
expect "refusals wrote nothing" "$(cmp "$T/before.img" "$T/part.img" && echo same)" "same"

head -c $((OFF + 8192)) "$B/../device/stock/boot_b.img" > "$T/stock.img"
out=$(BM_DEV="$T/stock.img" dash "$H" info 2>&1)
expect "stock boot refused" "$out" "update_helper: no boot menu in $T/stock.img (settings page)"
exit $fail
