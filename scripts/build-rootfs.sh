#!/usr/bin/env bash
# Build the Linux root filesystem image for the watch: Alpine Linux (aarch64) + OpenRC,
# Wi-Fi, SSH, NTP, and our UI and services. Run in WSL as builder (uses sudo):
#   wsl -d Ubuntu-24.04 -u builder -- bash /mnt/c/Github/rufus-cuff/scripts/build-rootfs.sh
# Output: images/linux/rootfs.img (ext4, label rufus-root). Install it with install-rootfs.sh.
# Needs qemu-user-static with the aarch64 binfmt registered (setup-wsl.sh).
set -euo pipefail

REPO=/mnt/c/Github/rufus-cuff
ALPINE=3.24.3
TARBALL=alpine-minirootfs-$ALPINE-aarch64.tar.gz
SHA256=4d3f80afaebd447c5591a63a73bde380a149fe8b2d85488cafabb2ff97551f50
MIRROR=https://dl-cdn.alpinelinux.org/alpine
DL=$HOME/port/dl
ROOT=$HOME/port/rootfs
IMG=$REPO/images/linux/rootfs.img
SIZE=2G
# Time zone (Android's: adb shell getprop persist.sys.timezone).
TZ_NAME=${TZ_NAME:-UTC}
PACKAGES="openrc busybox-openrc busybox-extras dropbear dropbear-openrc chrony chrony-openrc
	wpa_supplicant wpa_supplicant-openrc iw wireless-regdb e2fsprogs ca-certificates tzdata
	nano htop i2c-tools
	sxmo-utils sxmo-utils-sway seatd seatd-openrc font-dejavu grim eudev eudev-openrc
	util-linux-misc alsa-utils bluez bluez-openrc bluez-btmgmt bluez-deprecated"

[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] ||
	sudo /usr/lib/systemd/systemd-binfmt /usr/lib/binfmt.d/qemu-aarch64.conf
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || { echo "no aarch64 binfmt"; exit 1; }

mkdir -p "$DL"
[ -f "$DL/$TARBALL" ] || curl -sfL -o "$DL/$TARBALL" "$MIRROR/v${ALPINE%.*}/releases/aarch64/$TARBALL"
echo "$SHA256  $DL/$TARBALL" | sha256sum -c --quiet

sudo rm -rf "$ROOT"
sudo mkdir -p "$ROOT"
sudo tar -xzf "$DL/$TARBALL" -C "$ROOT"
sudo cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
printf '%s\n' "$MIRROR/v${ALPINE%.*}/main" "$MIRROR/v${ALPINE%.*}/community" |
	sudo tee "$ROOT/etc/apk/repositories" > /dev/null
in_root() { sudo chroot "$ROOT" /bin/sh -c "$1"; }
# Under qemu-user, apk cannot set setuid/setgid bits ("failed to preserve ... permission")
# and counts each as an error. Check that every package is in, then set those modes from
# apk's database (a:uid:gid:mode after each R:file).
in_root "apk add --no-cache --quiet $(echo $PACKAGES)" || echo "apk errors (setuid under qemu), checking"
in_root "apk info -e $(echo $PACKAGES) > /dev/null"
in_root "awk -F: '/^F:/ { d = substr(\$0, 3) } /^R:/ { f = substr(\$0, 3) }
	/^a:/ && \$4 ~ /^[246][0-7][0-7][0-7]\$/ { print \$4, d \"/\" f }' /lib/apk/db/installed |
	while read -r m p; do chmod \"\$m\" \"/\$p\"; done"

# ccci-fsd, the modem's NVRAM file service (itsvic-dev/ccci-fsd, the commit the AGM M7 port
# uses), built in the chroot with a temporary Rust toolchain that is removed again.
CCCI_FSD_REV=2e3fea9f4376e5c36eee1b495879840b7918e5b4
[ -d "$DL/ccci-fsd" ] || git clone -q https://github.com/itsvic-dev/ccci-fsd.git "$DL/ccci-fsd"
git -C "$DL/ccci-fsd" checkout -q "$CCCI_FSD_REV"
sudo rm -rf "$ROOT/tmp/ccci-fsd"
sudo cp -r "$DL/ccci-fsd" "$ROOT/tmp/ccci-fsd"
in_root "apk add --no-cache --quiet --virtual .ccci-build cargo" || echo "apk errors (setuid under qemu), checking"
in_root "command -v cargo > /dev/null && cd /tmp/ccci-fsd && cargo build --release --locked --quiet &&
	install -m 755 target/release/ccci-fsd /usr/local/bin/ccci-fsd"
in_root "apk del --quiet .ccci-build; rm -rf /tmp/ccci-fsd /root/.cargo" || true
[ -x "$ROOT/usr/local/bin/ccci-fsd" ] || { echo "ccci-fsd missing"; exit 1; }

# Our config and scripts (port/rootfs, text only: CRLF stripped in a copy, never on binaries).
overlay=$HOME/port/rootfs-overlay
rm -rf "$overlay"
cp -r "$REPO/port/rootfs" "$overlay"
find "$overlay" -type f -exec sed -i 's/\r$//' {} +
sudo cp -r "$overlay/." "$ROOT/"
sudo chmod 755 "$ROOT"/etc/init.d/rufus-* "$ROOT"/usr/local/bin/*
# Our programs (static, built by build-port.sh).
sudo install -m 755 "$HOME/port/initramfs/bin/pupui" "$HOME/port/initramfs/bin/evtest" \
	"$HOME/port/initramfs/bin/tap" "$HOME/port/initramfs/bin/disptool" \
	"$HOME/port/initramfs/bin/vibrate" "$HOME/port/initramfs/bin/rufus-reboot" \
	"$HOME/port/initramfs/bin/battery" "$ROOT/usr/local/bin/"
# Wi-Fi firmware, Wi-Fi and BT NVRAM from this watch (port/firmware/README.txt; never published).
sudo mkdir -p "$ROOT/lib/firmware"
sudo cp "$REPO"/port/firmware/ROMv2_lm_patch_1_*_hdr.bin "$REPO/port/firmware/WIFI_RAM_CODE_6755" \
	"$REPO/port/firmware/mt6739-wlan-nvram.bin" "$REPO/port/firmware/mt6739-bt-nvram.bin" \
	"$ROOT/lib/firmware/"
# Built binaries must arrive unmodified.
for b in pupui evtest tap disptool vibrate rufus-reboot; do
	cmp -s "$HOME/port/initramfs/bin/$b" "$ROOT/usr/local/bin/$b" || { echo "$b differs"; exit 1; }
done

# Services. sysinit/boot: the basics; default: ours plus network services; shutdown:
# stop everything and remount / read-only before the reboot.
# udev: libinput (the desktop's input) only uses devices that udev has tagged.
in_root "for s in devfs dmesg procfs sysfs udev udev-trigger udev-settle; do rc-update add \$s sysinit; done
	rc-update add udev-postmount default
	for s in bootmisc hostname localmount syslog rufus-clock; do rc-update add \$s boot; done
	for s in killprocs savecache mount-ro; do rc-update add \$s shutdown; done
	for s in rufus-wifi wpa_supplicant rufus-dhcp chronyd dropbear rufus-ui rufus-battery rufus-failsafe seatd rufus-audio rufus-modem; do rc-update add \$s default; done
	passwd -l root >/dev/null
	# The grown-up desktop's user (rufus-desktop); no password logins.
	adduser -D -s /bin/sh rufus && passwd -l rufus >/dev/null
	for g in video input seat audio; do addgroup rufus \$g; done
	ln -sf /usr/share/zoneinfo/$TZ_NAME /etc/localtime && echo $TZ_NAME > /etc/timezone"
sudo rm -f "$ROOT/etc/resolv.conf"

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
sudo mkfs.ext4 -q -F -L rufus-root -d "$ROOT" "$IMG" "$SIZE"
sudo chown "$(id -u):$(id -g)" "$IMG"
ls -l "$IMG"
sha256sum "$IMG"
