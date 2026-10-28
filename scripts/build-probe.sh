#!/usr/bin/env bash
# Build the LK boot probe as a recovery image: probe Image.gz + our DTB, same header as the real kernel.
#   wsl -d Ubuntu-24.04 -u builder -- bash /mnt/c/Github/rufus-cuff/scripts/build-probe.sh
set -euo pipefail
REPO=/mnt/c/Github/rufus-cuff
W=$HOME/port/probe; mkdir -p "$W"; cd "$W"
sed 's/\r$//' "$REPO/port/probe/probe.S" > probe.S
aarch64-linux-gnu-as -o probe.o probe.S
aarch64-linux-gnu-ld -Ttext=0 -o probe.elf probe.o
aarch64-linux-gnu-objcopy -O binary probe.elf Image
gzip -9 -n -c Image > Image.gz
aarch64-linux-gnu-objdump -d probe.elf > probe.dis
echo "Image $(stat -c %s Image) bytes, magic: $(dd if=Image bs=1 skip=56 count=4 2>/dev/null)"
# A minimal initramfs so LK sees a non-empty ramdisk, as with the real image.
printf 'probe\n' > probe.txt; echo probe.txt | cpio -o -H newc 2>/dev/null | gzip -9 > ramdisk.cpio.gz
python3 "$HOME/src/nixos-agm-m7/scripts/mkboot.py" --kernel Image.gz \
	--dtb "$REPO/images/linux/mt6739-rufus-cuff.dtb" --ramdisk ramdisk.cpio.gz \
	--cmdline "rufus.probe=1" -o "$REPO/images/linux/recovery-probe.img"
ls -l "$REPO/images/linux/recovery-probe.img"

# Probe 3: pad the image with 640 KiB of random data so the compressed blob is > 512 KiB.
# LK's 64-bit DTB lookup reads the last 512 KiB of the kernel blob; a smaller blob makes
# its read offset negative (found by disassembling LK at 0x56024b6e).
python3 - <<'PY'
import os, struct
img = bytearray(open('Image', 'rb').read())
img += os.urandom(640 * 1024)
struct.pack_into('<Q', img, 0x10, len(img))   # image_size
open('Image-pad', 'wb').write(img)
PY
gzip -9 -n -c Image-pad > Image-pad.gz
python3 "$HOME/src/nixos-agm-m7/scripts/mkboot.py" --kernel Image-pad.gz \
	--dtb "$REPO/images/linux/mt6739-rufus-cuff.dtb" --ramdisk ramdisk.cpio.gz \
	--cmdline "rufus.probe=3" -o "$REPO/images/linux/recovery-probe3.img"
ls -l Image-pad Image-pad.gz "$REPO/images/linux/recovery-probe3.img"

# Probe 32: 32-bit zImage-style probe for LK's stock 32-bit path (bootopt 32S1, kernel at 0x40008000).
sed 's/\r$//' "$REPO/port/probe/probe32.S" > probe32.S
arm-linux-gnueabihf-as -march=armv7-a -o probe32.o probe32.S
arm-linux-gnueabihf-ld -Ttext=0 -o probe32.elf probe32.o
arm-linux-gnueabihf-objcopy -O binary probe32.elf zImage-probe
echo "zImage-probe $(stat -c %s zImage-probe) bytes, magic@0x24: $(od -An -tx4 -j36 -N4 zImage-probe), end@0x2c: $(od -An -tu4 -j44 -N4 zImage-probe)"
python3 "$HOME/src/nixos-agm-m7/scripts/mkboot.py" --arm32 --kernel zImage-probe \
	--dtb "$REPO/images/linux/mt6739-rufus-cuff.dtb" --ramdisk ramdisk.cpio.gz \
	--cmdline "rufus.probe=32" -o "$REPO/images/linux/recovery-probe32.img"
ls -l "$REPO/images/linux/recovery-probe32.img"

# Probe 32s: the 32-bit probe with the stock DTB (rules out LK rejecting our DTB).
python3 "$HOME/src/nixos-agm-m7/scripts/mkboot.py" --arm32 --kernel zImage-probe \
	--dtb "$REPO/images/linux/stock-recovery.dtb" --ramdisk ramdisk.cpio.gz \
	--cmdline "rufus.probe=32s" -o "$REPO/images/linux/recovery-probe32s.img"
ls -l "$REPO/images/linux/recovery-probe32s.img"
