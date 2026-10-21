#!/bin/bash
# Back up chosen paths of the phone (read-only on the phone), one tar per path, absolute
# layout kept (tar -C /). Verification per tar: regular-file count and total bytes on
# the phone (find | xargs stat) against the tar's own listing on the PC, plus tar errors.
# Resumable: a tar that passed verification is skipped.
#   backup_selected.sh OUT_DIR
set -u
export MSYS_NO_PATHCONV=1
A=/d/Android/platform-tools/adb.exe
OUT=$1
mkdir -p "$OUT"
"$A" push "$(dirname "$0")/bkstat.sh" /data/local/tmp/bkstat.sh >/dev/null || exit 1
LOG="$OUT/verify.log"

ITEMS=(
	"photos_DCIM|/data/media/0/DCIM"
	"photos_Pictures|/data/media/0/Pictures"
	"photos_gallery_files|/data/media/0/MIUI/Gallery"
	"photos_gallery_app|/data/data/com.miui.gallery"
	"wechat_data|/data/data/com.tencent.mm"
	"wechat_de|/data/user_de/0/com.tencent.mm"
	"wechat_files|/data/media/0/Android/data/com.tencent.mm"
	"wechat_apk|APK:com.tencent.mm"
	"qq_data|/data/data/com.tencent.mobileqq"
	"qq_de|/data/user_de/0/com.tencent.mobileqq"
	"qq_files|/data/media/0/Android/data/com.tencent.mobileqq"
	"qq_apk|APK:com.tencent.mobileqq"
	"launcher_data|/data/data/com.miui.home"
	"launcher_de|/data/user_de/0/com.miui.home"
	"theme_app|/data/data/com.android.thememanager"
	"theme_de|/data/user_de/0/com.android.thememanager"
	"theme_applied|/data/system/theme"
	"wallpaper_app|/data/data/com.miui.miwallpaper"
	"settings_system|/data/system"
	"settings_system_ce|/data/system_ce/0"
	"settings_system_de|/data/system_de/0"
	"settings_misc|/data/misc"
	"settings_misc_ce|/data/misc_ce/0"
	"settings_misc_de|/data/misc_de/0"
	"settings_app|/data/data/com.android.settings"
	"settings_misettings|/data/data/com.xiaomi.misettings"
	"ime_sogou|/data/data/com.sohu.inputmethod.sogou.xiaomi"
	"ime_sogou_de|/data/user_de/0/com.sohu.inputmethod.sogou.xiaomi"
	"ime_sogou_files|/data/media/0/sogou"
	"ime_baidu|/data/data/com.baidu.input_mi"
	"ime_iflytek|/data/data/com.iflytek.inputmethod.miui"
)

ph() { "$A" shell "su -c '$1'" < /dev/null | tr -d '\r'; }

echo "$(date '+%F %T') start, ${#ITEMS[@]} items" | tee -a "$LOG"
fail=0
for item in "${ITEMS[@]}"; do
	label=${item%%|*} path=${item#*|}
	tarf="$OUT/$label.tar"
	case "$tarf" in "$OUT"/*) ;; *) echo "bad path $tarf"; exit 1;; esac
	[ -f "$tarf.ok" ] && continue
	if [[ $path == APK:* ]]; then
		path=$(ph "ls -d /data/app/*/${path#APK:}-* 2>/dev/null" | head -1)
		[ -n "$path" ] || { echo "$label: no apk dir" | tee -a "$LOG"; continue; }
	fi
	if [ -z "$(ph "[ -e $path ] && echo yes")" ]; then
		echo "$(date '+%T') skip $label: $path does not exist" | tee -a "$LOG"
		continue
	fi
	read -r pf pb < <(ph "sh /data/local/tmp/bkstat.sh $path")
	t0=$(date +%s)
	"$A" exec-out "su -c 'tar -cf - -C / ${path#/} 2>/data/local/tmp/bksel.err'" < /dev/null > "$tarf"
	t1=$(date +%s)
	err=$(ph "cat /data/local/tmp/bksel.err")
	read -r tf tb < <(tar -tvf "$tarf" 2>/dev/null | awk '$1 ~ /^-/ { n++; s += $3 } END { printf "%d %d\n", n, s }')
	size=$(stat -c %s "$tarf"); secs=$((t1 - t0)); [ $secs -gt 0 ] || secs=1
	if [ "$pf" = "$tf" ] && [ "$pb" = "$tb" ] && [ -z "$err" ]; then st=ok; : > "$tarf.ok"; else st=CHECK; fail=$((fail + 1)); fi
	printf '%s %s %-22s phone %s files %s bytes | tar %s files %s bytes | %s MB, %s MB/s | %s | err: %s\n' \
		"$(date '+%T')" "$st" "$label" "$pf" "$pb" "$tf" "$tb" $((size >> 20)) $(( (size >> 20) / secs )) "$path" "${err:-none}" | tee -a "$LOG"
done
echo "$(date '+%F %T') done, items needing a look: $fail" | tee -a "$LOG"
