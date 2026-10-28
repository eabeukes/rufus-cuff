#!/usr/bin/env bash
# Build the mainline kernel + initramfs boot image for the Rufus Cuff. Run in WSL as builder:
#   wsl -d Ubuntu-24.04 -u builder -- bash /mnt/c/Github/rufus-cuff/scripts/build-port.sh
# Output: images/linux/recovery-linux.img (to be written to the `recovery` partition).
set -euo pipefail

REPO=/mnt/c/Github/rufus-cuff
KSRC=$HOME/src/linux-mt6739
KOUT=$HOME/port/out
BUSYBOX=$HOME/port/dl/bb/usr/bin/busybox
MKBOOT=$HOME/src/nixos-agm-m7/scripts/mkboot.py
DTS=mt6739-rufus-cuff
OUT=$REPO/images/linux
CMDLINE=""   # the kernel command line is built in (CONFIG_CMDLINE_FORCE); LK keeps only ~100 chars of this field

kmake() { make -C "$KSRC" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j"$(nproc)" "$@"; }

# Our drivers (port/kernel mirrors the tree) and their Makefile hooks.
(cd "$REPO/port/kernel" && find . -name '*.c' -o -name '*.h') | while read -r f; do
	sed 's/\r$//' "$REPO/port/kernel/$f" > "$KSRC/$f"
done
hook() { grep -qF "$2" "$KSRC/$1" || echo "$2" >> "$KSRC/$1"; }
hook drivers/soc/mediatek/Makefile 'obj-$(CONFIG_ARCH_MEDIATEK) += mt6739-rdma-scanout.o'
hook drivers/video/backlight/Makefile 'obj-$(CONFIG_BACKLIGHT_CLASS_DEVICE) += aw9364-backlight.o'
hook drivers/iio/accel/Makefile 'obj-$(CONFIG_IIO) += bma423.o'
hook drivers/bluetooth/Makefile 'obj-$(CONFIG_BT) += btmt6739.o'

# Our device tree and config fragment into the kernel tree.
sed 's/\r$//' "$REPO/port/dts/$DTS.dts" > "$KSRC/arch/arm64/boot/dts/mediatek/$DTS.dts"
grep -q "$DTS.dtb" "$KSRC/arch/arm64/boot/dts/mediatek/Makefile" ||
	echo "dtb-\$(CONFIG_ARCH_MEDIATEK) += $DTS.dtb" >> "$KSRC/arch/arm64/boot/dts/mediatek/Makefile"
sed 's/\r$//' "$REPO/port/rufus-cuff.config" > "$KSRC/arch/arm64/configs/rufus-cuff.config"

kmake allnoconfig > /dev/null
kmake rufus-cuff.config > /dev/null
missing=0
while read -r line; do
	case $line in ''|'#'*) continue ;; esac
	grep -qxF "$line" "$KOUT/.config" || { echo "config not applied: $line"; missing=1; }
done < "$KSRC/arch/arm64/configs/rufus-cuff.config"
[ $missing = 0 ] || exit 1

kmake Image.gz "mediatek/$DTS.dtb" 2>&1 | grep -E "error|warning: .*$DTS|Error" || true
ls -l "$KOUT/arch/arm64/boot/Image.gz" "$KOUT/arch/arm64/boot/dts/mediatek/$DTS.dtb"

# Initramfs: static busybox + /init.
root=$HOME/port/initramfs
rm -rf "$root" && mkdir -p "$root/bin"
cp "$BUSYBOX" "$root/bin/busybox"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/disptool" "$REPO/port/tools/disptool.c"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/evtest" "$REPO/port/tools/evtest.c"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/tap" "$REPO/port/tools/tap.c"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/vibrate" "$REPO/port/tools/vibrate.c"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/rufus-reboot" "$REPO/port/tools/rufus-reboot.c"
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/mapfile" "$REPO/port/tools/mapfile.c"
# The same tool for Android (32-bit userspace), used by install-rootfs.sh.
mkdir -p "$OUT"
arm-linux-gnueabihf-gcc -static -O2 -Wall -o "$OUT/mapfile-android" "$REPO/port/tools/mapfile.c"
chmod 755 "$root/bin/disptool" "$root/bin/evtest"
# Kid UI and grown-up mode. Its art is generated at build time (assets.c is ~7 MB and not checked in).
mkdir -p "$HOME/port/pupui"
for f in pupui.c grownup.c theme.c ui.h; do
	sed 's/\r$//' "$REPO/port/pupui/$f" > "$HOME/port/pupui/$f"
done
python3 "$REPO/port/pupui/gen_assets.py" "$HOME/port/pupui/assets" > /dev/null
(cd "$HOME/port/pupui" &&
	aarch64-linux-gnu-gcc -static -O2 -Wall -o "$root/bin/pupui" pupui.c grownup.c theme.c assets.c -lm)
# Wi-Fi firmware + NVRAM (from the watch itself; port/firmware/README.txt).
mkdir -p "$root/lib/firmware"
cp "$REPO"/port/firmware/ROMv2_lm_patch_1_*_hdr.bin "$REPO/port/firmware/WIFI_RAM_CODE_6755" 	"$REPO/port/firmware/mt6739-wlan-nvram.bin" "$root/lib/firmware/"
# iw from Alpine (musl): loader + libnl, enough for "iw dev wlan0 scan".
A=$HOME/port/alpine/root
mkdir -p "$root/usr/lib"
cp "$A/lib/ld-musl-aarch64.so.1" "$root/lib/"
ln -sf ld-musl-aarch64.so.1 "$root/lib/libc.musl-aarch64.so.1"
cp "$A/usr/lib/libnl-3.so.200.26.0" "$root/usr/lib/libnl-3.so.200"
cp "$A/usr/lib/libnl-genl-3.so.200.26.0" "$root/usr/lib/libnl-genl-3.so.200"
cp "$A/usr/sbin/iw" "$root/bin/iw"
sed 's/\r$//' "$REPO/port/initramfs/init" > "$root/init"
chmod 755 "$root/init" "$root/bin/busybox"
sed 's/\r$//' "$REPO/port/initramfs/battery" > "$root/bin/battery"; chmod 755 "$root/bin/battery"
(cd "$root" && find . | cpio -o -H newc --owner=0:0 2>/dev/null | gzip -9) > "$HOME/port/initramfs.cpio.gz"

mkdir -p "$OUT"
python3 "$MKBOOT" --kernel "$KOUT/arch/arm64/boot/Image.gz" \
	--dtb "$KOUT/arch/arm64/boot/dts/mediatek/$DTS.dtb" \
	--ramdisk "$HOME/port/initramfs.cpio.gz" \
	--cmdline "$CMDLINE" -o "$OUT/recovery-linux.img"
cp "$KOUT/arch/arm64/boot/dts/mediatek/$DTS.dtb" "$OUT/"
ls -l "$OUT"
md5sum "$OUT/recovery-linux.img"
