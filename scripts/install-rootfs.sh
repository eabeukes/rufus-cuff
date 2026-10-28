#!/usr/bin/env bash
# Install the Linux root filesystem image on the watch, from Android over adb:
#   install-rootfs.sh [images/linux/rootfs.img]
# Puts it at /data/linux/rootfs.img and records its extents (as Android's ext4 reports them)
# in /data/linux/rootfs.map; Linux's initramfs maps exactly those blocks and refuses to boot
# from the image if its own view differs. Replacing the image replaces the map. Linux's
# changes live inside the image: `adb pull /data/linux/rootfs.img` is a full backup.
# To stop booting into it, delete /data/linux/rootfs.map.
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-adb}
here=$(dirname "$0")
img=${1:-$here/../images/linux/rootfs.img}
tool=$here/../images/linux/mapfile-android
[ -f "$img" ] && [ -f "$tool" ] || { echo "build first: build-rootfs.sh and build-port.sh"; exit 1; }

local_md5=$(md5sum "$img" | cut -c1-32)
"$ADB" shell 'mkdir -p /data/linux && rm -f /data/linux/rootfs.map /data/linux/rootfs.img.new'
"$ADB" push "$tool" /data/linux/mapfile > /dev/null
"$ADB" push "$img" /data/linux/rootfs.img.new
"$ADB" shell "cd /data/linux && chmod 755 mapfile &&
	[ \$(md5sum rootfs.img.new | cut -c1-32) = $local_md5 ] &&
	mv rootfs.img.new rootfs.img && sync &&
	./mapfile print rootfs.img > rootfs.map.new && mv rootfs.map.new rootfs.map && sync &&
	echo \"installed: \$(head -1 rootfs.map), \$((\$(wc -l < rootfs.map) - 1)) extents, md5 $local_md5\""
