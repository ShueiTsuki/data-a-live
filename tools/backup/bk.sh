#!/system/bin/sh
# Phone-side helper for backup_userdata.sh. Runs as root. Only reads.
#   bk.sh list TAG DIR      write the entries of DIR (one per line, hidden ones too) to
#                           /data/local/tmp/bk_TAG.list and print "INDEX<TAB>NAME"
#   bk.sh stat TAG INDEX    "FILES BYTES" of the regular files under entry INDEX
#   bk.sh tar  TAG INDEX    tar of entry INDEX on stdout; tar's errors go to
#                           /data/local/tmp/bk_TAG_INDEX.err
# Names are kept on the phone and addressed by index, so names with spaces or
# non-ASCII characters never pass through the PC's command line.
set -u
L=/data/local/tmp
cmd=$1 tag=$2
case "$cmd" in
list)
	dir=$3
	: > "$L/bk_$tag.list"
	echo "$dir" > "$L/bk_$tag.dir"
	cd "$dir" || exit 1
	for n in * .[!.]* ..?*; do
		[ -e "$n" ] || [ -L "$n" ] || continue
		printf '%s\n' "$n" >> "$L/bk_$tag.list"
	done
	awk '{ printf "%d\t%s\n", NR, $0 }' "$L/bk_$tag.list"
	;;
stat|tar)
	idx=$3
	dir=$(cat "$L/bk_$tag.dir")
	name=$(sed -n "${idx}p" "$L/bk_$tag.list")
	[ -n "$name" ] || { echo "no entry $idx" >&2; exit 1; }
	cd "$dir" || exit 1
	if [ "$cmd" = stat ]; then
		files=$(find "./$name" -type f | wc -l)
		# xargs splits the list itself; find -exec {} + overflowed the argument limit on big trees.
		bytes=$(find "./$name" -type f -print0 | xargs -0 stat -c %s | awk '{ s += $1 } END { printf "%d", s }')
		echo "$files ${bytes:-0}"
	else
		tar -cf - "./$name" 2> "$L/bk_${tag}_$idx.err"
	fi
	;;
*)
	echo "usage: bk.sh list TAG DIR | stat TAG INDEX | tar TAG INDEX" >&2
	exit 2
	;;
esac
