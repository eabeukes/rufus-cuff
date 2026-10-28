#!/usr/bin/env bash
# Write a boot image to the watch's boot partition (mmcblk0p25) over adb and verify it.
# Layout since 2026-10-28: Linux in boot (LK's normal boot loads the modem), stock Android
# in recovery (scripts/flash-recovery.sh images/linux/android-in-recovery.img).
#   flash-boot.sh <image>      (sets the header id hash first)
# The current boot partition is saved to images/boot-before-<timestamp>.img first.
# Undo: flash-boot.sh images/boot-fast/boot-slub-off.img (Android) and flash-recovery.sh with
# the Linux image; the factory boot is backup-20261027/boot.img.
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-adb}
here=$(dirname "$0")
img=$1; tmp="$img.id-tmp"   # next to the image: Windows Python can't see Git Bash's /tmp
python -I "$here/bootimg_cmdline.py" patch "$img" "$tmp" "" > /dev/null
[ -s "$tmp" ] || { echo "id patch produced no output"; exit 1; }
mv "$tmp" "$img"
python -I "$here/bootimg_cmdline.py" show "$img" | sed -n 2p

save="$here/../images/boot-before-$(date +%Y%m%d-%H%M%S).img"
"$ADB" shell 'P=$(readlink -f /dev/block/platform/bootdevice/by-name/boot); [ "$P" = /dev/block/mmcblk0p25 ] || exit 1; dd if=$P of=/data/local/tmp/b-old.img bs=1048576 2>/dev/null'
"$ADB" pull /data/local/tmp/b-old.img "$save" > /dev/null
"$ADB" shell rm /data/local/tmp/b-old.img
echo "saved the old boot partition to $save"

local_md5=$(md5sum "$img" | cut -c1-32)
"$ADB" push "$img" /data/local/tmp/b.img > /dev/null
dev_md5=$("$ADB" shell 'P=$(readlink -f /dev/block/platform/bootdevice/by-name/boot); [ "$P" = /dev/block/mmcblk0p25 ] || exit 1; dd if=/data/local/tmp/b.img of=$P bs=1048576 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches; N=$(stat -c %s /data/local/tmp/b.img); dd if=$P bs=2048 count=$((N/2048)) 2>/dev/null | md5sum; rm /data/local/tmp/b.img' | cut -c1-32)
[ "$local_md5" = "$dev_md5" ] && echo "boot written, md5 $dev_md5 verified" || { echo "MD5 MISMATCH local=$local_md5 device=$dev_md5"; exit 1; }
