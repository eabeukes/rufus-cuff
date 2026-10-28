#!/usr/bin/env bash
# Provision the Ubuntu 24.04 WSL build host for the port. Run as root:
#   wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/Github/rufus-cuff/scripts/setup-wsl.sh
# Re-runnable.
set -euo pipefail

BUILD_USER=builder
SRC=/home/$BUILD_USER/src

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf bc bison flex libssl-dev \
  libelf-dev device-tree-compiler mkbootimg cpio lz4 xz-utils zstd kmod git python3 \
  python3-venv python3-pil pipx libusb-1.0-0 busybox-static u-boot-tools sudo \
  qemu-user-static binfmt-support

# Run aarch64 binaries in chroots (build-rootfs.sh). WSL does not start systemd-binfmt itself.
/usr/lib/systemd/systemd-binfmt /usr/lib/binfmt.d/qemu-aarch64.conf

id "$BUILD_USER" >/dev/null 2>&1 || useradd -m -s /bin/bash "$BUILD_USER"
echo "$BUILD_USER ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$BUILD_USER
printf '[user]\ndefault=%s\n' "$BUILD_USER" > /etc/wsl.conf

sudo -u "$BUILD_USER" bash -s <<EOF
set -euo pipefail
# PyPI releases are yanked; postmarketOS ships pmbootstrap from its GitLab.
pipx install --quiet --force git+https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git
mkdir -p $SRC && cd $SRC
[ -d linux-mt6739 ] || git clone -q --depth 1 -b linux-7.2 https://github.com/itsvic-dev/linux-mt6739.git
[ -d u-boot-mt6739 ] || git clone -q --depth 1 -b agm-m7 https://github.com/itsvic-dev/u-boot-mt6739.git
[ -d nixos-agm-m7 ] || git clone -q --depth 1 https://github.com/itsvic-dev/nixos-agm-m7.git
EOF

echo "OK: $(aarch64-linux-gnu-gcc --version | head -1)"
echo "OK: pmbootstrap $(sudo -u $BUILD_USER /home/$BUILD_USER/.local/bin/pmbootstrap --version)"
ls $SRC
