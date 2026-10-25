#!/bin/bash
# Builds the iOS 16 root image for the S9 inside WSL (run as root by
# build-ios-root.bat). Steps: check HFS+ support, install tools, fetch the
# three disk images from Apple's iPhone X IPSW (range requests, ~5 GB instead
# of the whole 6 GB file), tools/mkiosroot.sh, tools/sweet-root-tweaks.py.
# Result: /root/ios-root/ios16-root.img (16 GiB HFS+). Re-running resumes:
# finished steps are skipped. Log: ios-root.log next to this script.
set -euo pipefail

IPSW_URL="https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw"
WORK=/root/ios-root
IMG=$WORK/ios16-root.img
SIZE=16G

step() { echo; echo "=== $(date +%H:%M:%S) $*"; }

step "1/7 HFS+ support in the WSL kernel"
uname -r
if ! grep -qw hfsplus /proc/filesystems; then
	modprobe hfsplus 2>&1 || true
fi
if ! grep -qw hfsplus /proc/filesystems; then
	echo "RESULT: NO_HFSPLUS (this WSL kernel cannot mount HFS+)"
	exit 3
fi
echo "hfsplus OK"
[ -e /dev/fuse ] || { echo "RESULT: NO_FUSE"; exit 4; }
echo "fuse OK"

step "2/7 Tools"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q hfsprogs rsync curl git python3 sudo fuse3 >/dev/null
if ! command -v apfs-fuse >/dev/null; then
	apt-get install -y -q apfs-fuse >/dev/null 2>&1 || true
fi
if ! command -v apfs-fuse >/dev/null; then
	echo "building apfs-fuse from source"
	apt-get install -y -q cmake g++ libfuse3-dev zlib1g-dev libbz2-dev libattr1-dev liblzma-dev >/dev/null
	rm -rf /root/apfs-fuse
	git clone -q --recursive https://github.com/sgan81/apfs-fuse /root/apfs-fuse
	# New CMake refuses its old minimum version; new GCC wants <cstdint> spelled out.
	cmake -S /root/apfs-fuse -B /root/apfs-fuse/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint" \
		-DCMAKE_C_FLAGS="-Wno-error" >/dev/null
	make -C /root/apfs-fuse/build -j"$(nproc)" 2>&1 | grep -E "error|Error" || true
	[ -x /root/apfs-fuse/build/apfs-fuse ] || { echo "RESULT: APFS_FUSE_BUILD_FAILED"; exit 5; }
	install -m 755 /root/apfs-fuse/build/apfs-fuse /usr/local/bin/apfs-fuse
fi
command -v apfs-fuse mkfs.hfsplus fsck.hfsplus
mkdir -p "$WORK"
df -h "$WORK"

step "3/7 ios-sweet scripts"
if [ -d "$WORK/ios-sweet/.git" ]; then
	git -C "$WORK/ios-sweet" pull -q
else
	git clone -q --depth 1 https://github.com/alphanCZE/ios-sweet "$WORK/ios-sweet"
fi
T=$WORK/ios-sweet/tools

if [ ! -s "$IMG.done" ]; then
	step "4/7 Disk image names from BuildManifest.plist"
	mkdir -p "$WORK/ipsw"
	[ -s "$WORK/ipsw/BuildManifest.plist" ] || python3 "$T/remotezip.py" "$IPSW_URL" BuildManifest.plist "$WORK/ipsw"
	read -r OS SYSOS APPOS < <(python3 - "$WORK/ipsw/BuildManifest.plist" <<'EOF'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
for b in m['BuildIdentities']:
    info = b['Info']
    if info.get('DeviceClass', '').lower() == 'd22ap' and 'erase' in info.get('Variant', '').lower():
        bm = b['Manifest']
        print(bm['OS']['Info']['Path'], bm['Cryptex1,SystemOS']['Info']['Path'], bm['Cryptex1,AppOS']['Info']['Path'])
        break
else:
    sys.exit('no d22ap erase identity')
EOF
)
	echo "OS $OS / SystemOS $SYSOS / AppOS $APPOS"

	step "5/7 Downloading the three disk images (about 5 GB, resumes if stopped)"
	for f in "$OS" "$SYSOS" "$APPOS"; do
		[ -s "$WORK/ipsw/$f" ] || python3 "$T/remotezip.py" "$IPSW_URL" "$f" "$WORK/ipsw"
		ls -la "$WORK/ipsw/$f"
	done

	step "6/7 Building the HFS+ root ($SIZE)"
	rm -f "$IMG"
	bash "$T/mkiosroot.sh" "$WORK/ipsw/$OS" "$WORK/ipsw/$SYSOS" "$WORK/ipsw/$APPOS" "$IMG" "$SIZE"
	echo ok > "$IMG.done"
	rm -f "$WORK/ipsw/"*.dmg "$WORK/ipsw/"*.aea 2>/dev/null || true
fi

step "7/7 S9 bring-up tweaks"
fsck.hfsplus -fy "$IMG" || true
mkdir -p /mnt/iosroot
mount -t hfsplus -o loop,force "$IMG" /mnt/iosroot
python3 "$T/sweet-root-tweaks.py" /mnt/iosroot
df -h /mnt/iosroot
umount /mnt/iosroot
fsck.hfsplus -fn "$IMG"
ls -la "$IMG"
echo "RESULT: ROOT_IMAGE_READY $IMG"
