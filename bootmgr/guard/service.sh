#!/system/bin/sh
# Magisk late_start service (non-blocking): the guard for the whole Android session.
exec sh "${0%/*}/guard.sh"
