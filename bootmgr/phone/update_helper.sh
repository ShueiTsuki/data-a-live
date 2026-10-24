#!/system/bin/sh
# Phone side of bootmgr/update_ios.sh, run as root:
#   update_helper.sh info              boot partition, slot offset and size, pinned files
#   update_helper.sh write SLOTFILE    replace the iOS slot in boot_<slot> with SLOTFILE
# write touches only the slot: the header page is cleared first, then the Image is written,
# then the header (armed) last, so an interrupted write leaves an empty slot ("iOS off").
# Afterwards the slot is read back, and everything outside it must hash as before.
# POSIX sh (Android's mksh). BM_DEV / BM_PINFILE replace the partition / pinfile in the PC test.

DEV=${BM_DEV:-/dev/block/by-name/boot$(getprop ro.boot.slot_suffix)}
P=${BM_PINFILE:-/data/local/tmp/pinfile}
CFG=53248                       # 0xd000: the settings page (menu.c struct bm_config)

hex_at() {
	dd if="$DEV" bs=1 skip="$1" count="$2" 2>/dev/null | od -A n -t x1 -v | tr -d ' \n'
}

# little-endian u64 from 16 hex digits -> decimal
le64() {
	be=
	i=15
	while [ $i -ge 1 ]; do
		be=$be$(echo "$1" | cut -c$i-$((i + 1)))
		i=$((i - 2))
	done
	printf '%d' "0x$be"
}

die() {
	echo "update_helper: $*"
	exit 1
}

h=$(hex_at $CFG 32)
[ "$(echo "$h" | cut -c1-16)" = 504e44424d434632 ] || die "no boot menu in $DEV (settings page)"
SLOT=$((4096 + $(le64 "$(echo "$h" | cut -c33-48)")))
MAX=$(le64 "$(echo "$h" | cut -c49-64)")
END=$((SLOT + MAX))
[ $((SLOT % 4096)) -eq 0 ] && [ $((END % 4096)) -eq 0 ] || die "slot not page aligned ($SLOT $MAX)"
PART=$(blockdev --getsize64 "$DEV" 2>/dev/null || wc -c < "$DEV")
[ "$END" -le "$PART" ] || die "slot beyond the partition ($END > $PART)"

outside() {
	a=$(dd if="$DEV" bs=4096 count=$((SLOT / 4096)) 2>/dev/null | sha256sum | cut -c1-64)
	b=$(dd if="$DEV" bs=4096 skip=$((END / 4096)) 2>/dev/null | sha256sum | cut -c1-64)
	echo "$a$b"
}

case $1 in
info)
	echo "dev $DEV"
	echo "slot $SLOT $MAX"
	for f in iosroot ioslog adspfw; do
		"$P" status "/data/local/tmp/$f.img"
	done
	;;
write)
	F=$2
	[ -f "$F" ] || die "no $F"
	size=$(wc -c < "$F")
	[ "$size" -gt 4096 ] && [ "$size" -le "$MAX" ] || die "slot file $size bytes, room for $MAX"
	[ "$(dd if="$F" bs=8 count=1 2>/dev/null)" = PNDSLOT1 ] || die "$F is not a slot"
	before=$(outside)
	page=$((SLOT / 4096))
	dd if=/dev/zero of="$DEV" bs=4096 seek=$page count=1 conv=notrunc 2>/dev/null || die "clearing the header failed"
	sync
	dd if="$F" of="$DEV" bs=4096 skip=1 seek=$((page + 1)) conv=notrunc 2>/dev/null || die "writing the Image failed"
	sync
	dd if="$F" of="$DEV" bs=4096 count=1 seek=$page conv=notrunc 2>/dev/null || die "writing the header failed"
	sync
	blockdev --flushbufs "$DEV" 2>/dev/null
	want=$(sha256sum < "$F" | cut -c1-64)
	got=$(dd if="$DEV" bs=4096 skip=$page count=$(((size + 4095) / 4096)) 2>/dev/null | head -c "$size" |
		sha256sum | cut -c1-64)
	after=$(outside)
	[ "$got" = "$want" ] || die "READ BACK DIFFERS: slot $got, file $want"
	[ "$after" = "$before" ] || die "OUTSIDE THE SLOT CHANGED"
	echo "write ok: $size bytes at $SLOT in $DEV, outside unchanged"
	;;
*)
	die "usage: update_helper.sh info | write SLOTFILE"
	;;
esac
