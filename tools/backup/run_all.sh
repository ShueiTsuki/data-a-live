#!/bin/bash
# Full userdata backup, run in order: internal storage to E:, app data and settings to D:.
cd "$(dirname "$0")"
B=./backup_userdata.sh
bash $B sdcard   /data/media/0      /e/pandora_backup/sdcard
bash $B datadata /data/data         /d/pandora_backup/data_data
bash $B userde   /data/user_de/0    /d/pandora_backup/user_de
bash $B app      /data/app          /d/pandora_backup/app
for d in adb misc system; do bash $B sys_$d /data/$d /d/pandora_backup/sys_$d; done
for d in system_ce system_de misc_ce misc_de; do bash $B sys_$d /data/$d/0 /d/pandora_backup/sys_$d; done
echo ALL_DONE
