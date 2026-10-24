#!/system/bin/sh
# pandora boot menu guard (run by the Magisk module's service.sh).
#
# boot_<slot> may hold the boot menu (ios_pandora/bootmgr). Its settings page at partition
# offset 0xd000 gives the iOS slot's place; the slot's header page says whether volume up may
# start iOS ("armed") and holds, for iosroot, ioslog and adspfw in /data/local/tmp, the
# sha256 of `pinfile extents FILE --json` the slot's iOS was built for: the blocks it will
# read and write. At boot and then once a minute this checks that the three files are still
# pinned with exactly those extents. If not, it clears "armed" and writes the reason into the
# slot header; the menu then shows "iOS off: <reason>" and starts Android. It never arms
# again (update_ios.sh does, with a new slot) and writes nothing else.
#
#   guard.sh [--dev FILE] [--dir DIR] [--pinfile BIN] [--log FILE] [--once]
# (tests: a copy of the partition instead of boot_<slot>)
# POSIX sh: runs under Android's mksh, and under dash/bash for the PC test.

MOD=${0%/*}
DEV=
DIR=/data/local/tmp
P=$MOD/pinfile
LOG=$MOD/guard.log
ONCE=
while [ $# -gt 0 ]; do
	case $1 in
	--dev) DEV=$2; shift 2 ;;
	--dir) DIR=$2; shift 2 ;;
	--pinfile) P=$2; shift 2 ;;
	--log) LOG=$2; shift 2 ;;
	--once) ONCE=1; shift ;;
	*) echo "usage: guard.sh [--dev FILE] [--dir DIR] [--pinfile BIN] [--log FILE] [--once]" >&2; exit 2 ;;
	esac
done
[ -n "$DEV" ] || DEV=/dev/block/by-name/boot$(getprop ro.boot.slot_suffix)
CFG=53248                       # 0xd000: the settings page (menu.c struct bm_config)
CFG_MAGIC=504e44424d434632      # "PNDBMCF2"
SLOT_ARMED=504e44534c4f543101000000     # "PNDSLOT1", armed = 1 (little endian)
FILES="iosroot ioslog adspfw"   # order of the hashes in the slot header
OFF=                            # partition offset of the slot header, from the settings page

log() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"
	# keep the log small
	if [ "$(wc -c < "$LOG")" -gt 65536 ]; then
		tail -n 200 "$LOG" > "$LOG.new" && mv "$LOG.new" "$LOG"
	fi
}

# hex BYTES bytes at partition offset AT
hex_at() {
	dd if="$DEV" bs=1 skip="$1" count="$2" 2>/dev/null | od -A n -t x1 -v | tr -d ' \n'
}

# The slot header's offset from the settings page (slot_off, a little-endian u64 at +16), or
# nothing when boot_<slot> holds no boot menu.
find_slot() {
	h=$(hex_at $CFG 24)
	[ "$(echo "$h" | cut -c1-16)" = "$CFG_MAGIC" ] || return 1
	le=$(echo "$h" | cut -c33-48)
	be=
	i=15
	while [ $i -ge 1 ]; do
		be=$be$(echo "$le" | cut -c$i-$((i + 1)))
		i=$((i - 2))
	done
	OFF=$((4096 + $(printf '%d' "0x$be")))
}

# The first 128 bytes of the slot header as hex (magic, armed, size, sum, 3 x sha256).
page_hex() {
	hex_at $OFF 128
}

disarm() {
	# Only a page that is still ours and still armed is touched.
	[ "$(page_hex | cut -c1-24)" = "$SLOT_ARMED" ] || return 0
	dd if=/dev/zero of="$DEV" bs=1 seek=$((OFF + 8)) count=4 conv=notrunc 2>/dev/null
	dd if=/dev/zero of="$DEV" bs=1 seek=$((OFF + 128)) count=64 conv=notrunc 2>/dev/null
	printf '%s' "$1" | head -c 40 | dd of="$DEV" bs=1 seek=$((OFF + 128)) conv=notrunc 2>/dev/null
	sync
	if [ "$(page_hex | cut -c17-24)" = 00000000 ]; then
		log "iOS turned off in $DEV: $1"
	else
		log "FAILED to turn iOS off in $DEV ($1)"
	fi
}

LAST=
# One check; logs only when the result changes.
check() {
	state=
	if ! find_slot; then
		state="no boot menu in $DEV"
	else
		hex=$(page_hex)
		case $hex in
		"$SLOT_ARMED"*) ;;
		504e44534c4f5431*) state="iOS already off" ;;
		*) state="iOS slot empty" ;;
		esac
	fi
	if [ -z "$state" ]; then
		i=0
		for f in $FILES; do
			want=$(echo "$hex" | cut -c$((65 + i * 64))-$((128 + i * 64)))
			st=$("$P" status "$DIR/$f.img" 2>&1)
			case $st in
			*": pinned (GC move failures 0)"*) ;;
			*) disarm "$f not pinned"; state="turned off: $f not pinned ($st)"; break ;;
			esac
			got=$("$P" extents "$DIR/$f.img" --json 2>/dev/null | sha256sum | cut -c1-64)
			if [ "$got" != "$want" ]; then
				disarm "$f moved"
				state="turned off: $f moved"
				break
			fi
			i=$((i + 1))
		done
		[ -n "$state" ] || state="armed, iosroot ioslog adspfw in place"
	fi
	[ "$state" = "$LAST" ] || log "$state"
	LAST=$state
}

chmod 755 "$P" 2>/dev/null
while :; do
	check
	[ -n "$ONCE" ] && break
	sleep 60
done
