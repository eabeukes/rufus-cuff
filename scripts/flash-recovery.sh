#!/usr/bin/env bash
# Write a boot image to the watch's recovery partition (mmcblk0p2) over adb and verify it.
#   flash-recovery.sh <image>      (sets the header id hash first)
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-adb}
here=$(dirname "$0")
img=$1; tmp="$img.id-tmp"   # next to the image: Windows Python can't see Git Bash's /tmp
python -I "$here/bootimg_cmdline.py" patch "$img" "$tmp" "" > /dev/null
[ -s "$tmp" ] || { echo "id patch produced no output"; exit 1; }
mv "$tmp" "$img"
python -I "$here/bootimg_cmdline.py" show "$img" | sed -n 2p
local_md5=$(md5sum "$img" | cut -c1-32)
"$ADB" push "$img" /data/local/tmp/r.img > /dev/null
dev_md5=$("$ADB" shell 'P=$(readlink -f /dev/block/platform/bootdevice/by-name/recovery); [ "$P" = /dev/block/mmcblk0p2 ] || exit 1; dd if=/data/local/tmp/r.img of=$P bs=1048576 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches; N=$(stat -c %s /data/local/tmp/r.img); dd if=$P bs=2048 count=$((N/2048)) 2>/dev/null | md5sum; rm /data/local/tmp/r.img' | cut -c1-32)
[ "$local_md5" = "$dev_md5" ] && echo "recovery written, md5 $dev_md5 verified" || { echo "MD5 MISMATCH local=$local_md5 device=$dev_md5"; exit 1; }
