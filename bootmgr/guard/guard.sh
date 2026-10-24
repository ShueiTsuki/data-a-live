#!/system/bin/sh
# pandora boot menu guard (run by the Magisk module's service.sh).
#
# boot_<slot> may hold the boot menu (ios_pandora/bootmgr). Its settings page at partition
# offset 0xd000 says whether volume up may start iOS ("armed") and holds, for iosroot, ioslog
# and adspfw in /data/local/tmp, the sha256 of `pinfile extents FILE --json` taken when the
# image was built: the blocks iOS will read and write. At boot and then once a minute this
# checks that the three files are still pinned with exactly those extents. If not, it clears
# "armed" and writes the reason into the page; the menu then shows "iOS off: <reason>" and
# starts Android. It never arms again (that takes a new image) and writes nothing else.
#
#   guard.sh [--dev FILE] [--dir DIR] [--pinfile BIN] [--log FILE] [--once]
# (tests: a copy of the partition's first pages instead of boot_<slot>)
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
OFF=53248                       # 0xd000: header page + the menu's .config section
MAGIC_ARMED=504e44424d43463101000000    # "PNDBMCF1", armed = 1 (little endian)
FILES="iosroot ioslog adspfw"   # order of the hashes in the page

log() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"
	# keep the log small
	if [ "$(wc -c < "$LOG")" -gt 65536 ]; then
		tail -n 200 "$LOG" > "$LOG.new" && mv "$LOG.new" "$LOG"
	fi
}

# The first 120 bytes of the settings page as hex (magic, armed, wait, flags, 3 x sha256).
page_hex() {
	dd if="$DEV" bs=4096 skip=13 count=1 2>/dev/null | head -c 120 | od -A n -t x1 -v | tr -d ' \n'
}

disarm() {
	# Only a page that is still ours and still armed is touched.
	[ "$(page_hex | cut -c1-24)" = "$MAGIC_ARMED" ] || return 0
	dd if=/dev/zero of="$DEV" bs=1 seek=$((OFF + 8)) count=4 conv=notrunc 2>/dev/null
	dd if=/dev/zero of="$DEV" bs=1 seek=$((OFF + 120)) count=64 conv=notrunc 2>/dev/null
	printf '%s' "$1" | head -c 40 | dd of="$DEV" bs=1 seek=$((OFF + 120)) conv=notrunc 2>/dev/null
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
	hex=$(page_hex)
	state=
	case $hex in
	"$MAGIC_ARMED"*) ;;
	504e44424d434631*) state="boot menu present, iOS already off" ;;
	*) state="no boot menu in $DEV" ;;
	esac
	if [ -z "$state" ]; then
		i=0
		for f in $FILES; do
			want=$(echo "$hex" | cut -c$((49 + i * 64))-$((112 + i * 64)))
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
