set -e
H=/mnt/c/Users/MSI-PC/AppData/Local/Temp/claude/D--DeepSeekHarness-claude/9c9599fd-dd66-4798-99ee-ebdfbfa33bd2/scratchpad/cs4k
W=/opt/work/cs4k; rm -rf $W; mkdir -p $W; tr -d '\r' < $H/harness.c > $W/harness.c
cd /opt/work/tree
extract() { python3 - "$1" "$2" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('static int\ncs_native4k_prepare(vnode_t vp, struct cs_blob *blob)\n{')
b = s.index('\n}\n', a) + 3
open(sys.argv[2], 'w').write(s[a:b])
PY
}
git show HEAD~1:bsd/kern/ubc_subr.c > $W/ubc_old.c; extract $W/ubc_old.c $W/old.c
extract bsd/kern/ubc_subr.c $W/new.c
diff <(grep -c . $W/old.c) <(grep -c . $W/new.c) >/dev/null || true
cd $W; gcc -O2 -w -o t harness.c -lcrypto
M=/opt/work/iosroot/ro; mkdir -p $M
grep -qw hfsplus /proc/filesystems || insmod /opt/work/wslk/fs/hfsplus/hfsplus.ko
mount -t hfsplus -o loop,ro /opt/work/iosroot/ios-root-base.img $M
./t $(ls /opt/work/dsc/dyld_shared_cache_arm64* | grep -v symbols) > $W/all.txt 2>&1 || true; grep -c IDENTICAL $W/all.txt; grep -v IDENTICAL $W/all.txt; awk "{s+=$9; t+=$12} END {print \"old reads\", s, \"new reads\", t}" $W/all.txt

umount $M
