#!/system/bin/sh
# Print "FILES BYTES" of the regular files under $1 (phone side, read-only).
p=$1
printf '%s %s\n' "$(find "$p" -type f | wc -l)" \
	"$(find "$p" -type f -print0 | xargs -0 stat -c %s 2>/dev/null | awk '{ s += $1 } END { printf "%d", s }')"
