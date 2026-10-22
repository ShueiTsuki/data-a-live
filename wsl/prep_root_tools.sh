#!/bin/bash
# Prepare WSL for building the iOS HFS+ root (tools/mkiosroot from ios-fuxi):
#  1. hfsplus.ko for the running WSL kernel (the stock WSL kernel has no HFS+): build
#     microsoft/WSL2-Linux-Kernel at the matching tag with HFSPLUS_FS=m (MODVERSIONS is on,
#     so a full build is needed for Module.symvers).
#  2. apfs-fuse (sgan81) to read the IPSW's APFS images.
#  3. Extract OS / SystemOS / AppOS from the IPSW to /opt/work/ipsw (unzip checks CRCs).
# Clones use a mirror per command only (never a global url.insteadOf).
set -euo pipefail
MIRROR=https://ghfast.top/https://github.com/
IPSW="/mnt/d/ClaudeAre/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw"
W=/opt/work
log() { echo "== $(date +%T) $*"; }

log apt
DEBIAN_FRONTEND=noninteractive apt-get install -y -q libelf-dev dwarves cpio bc flex bison \
	libfuse3-dev fuse3 zlib1g-dev libbz2-dev libattr1-dev cmake g++ git >/dev/null

KVER=$(uname -r)                       # 6.6.87.2-microsoft-standard-WSL2
TAG=linux-msft-wsl-${KVER%%-microsoft*}
if [ ! -f $W/wslk/fs/hfsplus/hfsplus.ko ]; then
	log "kernel $TAG"
	[ -d $W/wslk ] || git -c url.$MIRROR.insteadOf=https://github.com/ clone -q --depth 1 -b $TAG \
		https://github.com/microsoft/WSL2-Linux-Kernel $W/wslk
	cd $W/wslk
	zcat /proc/config.gz > .config
	scripts/config --module HFSPLUS_FS --disable LOCALVERSION_AUTO
	make -s olddefconfig
	make -s -j"$(nproc)" LOCALVERSION= >$W/wslk-build.log 2>&1 || { tail -30 $W/wslk-build.log; exit 1; }
fi
modinfo -F vermagic $W/wslk/fs/hfsplus/hfsplus.ko
grep -qw hfsplus /proc/filesystems || insmod $W/wslk/fs/hfsplus/hfsplus.ko
grep -w hfsplus /proc/filesystems

if ! command -v apfs-fuse >/dev/null; then
	log apfs-fuse
	[ -d $W/apfs-fuse ] || git -c url.$MIRROR.insteadOf=https://github.com/ clone -q --recursive \
		https://github.com/sgan81/apfs-fuse $W/apfs-fuse
	cd $W/apfs-fuse && mkdir -p build && cd build
	cmake -DUSE_FUSE3=ON .. >/dev/null && make -s -j"$(nproc)" && install -m 755 apfs-fuse apfsutil /usr/local/bin/
fi
command -v apfs-fuse

log "ipsw images"
mkdir -p $W/ipsw && cd $W/ipsw
unzip -o -q "$IPSW" 087-86698-044.dmg 087-86593-047.dmg 087-86715-047.dmg \
	Firmware/all_flash/DeviceTree.d22ap.im4p BuildManifest.plist
ls -l $W/ipsw $W/ipsw/Firmware/all_flash
log done
