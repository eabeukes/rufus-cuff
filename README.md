# Rufus Cuff (TICWRIS MAX, MT6739, Android 7.1.1)

Firmware: `TICWRIS_MAX_V1.2_20200107`. The stock images and this watch's backup are kept
offline (not in this repo).

`ro.secure=0` on this build: `adb shell` is already root. No unlock or Magisk needed.

## Backup

`backup-20261027/` holds raw dumps of every partition except `userdata` and `cache`,
plus `SHA256SUMS`. `nvram`, `nvdata`, `nvcfg` and `proinfo` hold this unit's IMEI,
Wi-Fi/BT MACs and radio calibration. The stock firmware does not contain them. Keep a copy off this PC.

## Changes made 2026-10-27

- Disabled (`pm disable-user --user 0`) the 21 packages in `packages-disabled.txt`:
  Adups FOTA, MTK logger, Baidu location, Wiite app store / task killer / IMEI writer,
  face unlock, live wallpapers, and all Google apps (GMS, GSF, Play Store, Maps, Google app, TTS, ...).
- Animation scales set to 0.5.
- `boot` partition: header cmdline gets `slub_debug=-`, which switches off the kernel slab debugging that
  LK turns on (`slub_debug=OFZPU`). Boot ~13% faster, slab memory −40%, app cold starts ~17-20% faster.
  Image: `images/boot-fast/boot-slub-off.img`. To undo, `dd` the first 11231232 bytes of
  `backup-20261027/boot.img` back to `boot` (mmcblk0p25).

The bootloader boots unsigned `boot`/`recovery` images even though it reports "locked".
See `PORT-PLAN.md` for that, the unbrick path (SP Flash Tool) and the port plan.

## Mainline Linux (in `recovery`)

Linux 7.2.9 boots from the `recovery` partition (`adb reboot recovery`; the next reset is Android
again) into Alpine Linux with display, backlight, touch, buttons, charging and fuel gauge, clock,
Wi-Fi, SSH, the kid UI "Pup Rescue HQ" (`port/pupui/`) and a grown-up mode behind it. Status: `PORT-PLAN.md`; history:
`docs/JOURNAL.md`.

Build (WSL) and install (Git Bash, watch in Android with adb):

```
wsl -d Ubuntu-24.04 -u builder -- bash /mnt/c/Github/rufus-cuff/scripts/build-port.sh
wsl -d Ubuntu-24.04 -u builder -- env TZ_NAME=Europe/London bash /mnt/c/Github/rufus-cuff/scripts/build-rootfs.sh
scripts/install-rootfs.sh        # /data/linux/rootfs.img (2 GiB) + its extent map
cp images/linux/recovery-linux.img images/linux/boot-linux.img
scripts/flash-boot.sh images/linux/boot-linux.img   # Linux in boot (since 2026-10-28)
adb reboot
```

Since 2026-10-28 Linux is in `boot` (so LK loads the modem) and stock Android is in `recovery`:
power-on starts Linux; "back to Android" (3 s hold, grown-up menu, low battery) reboots into
recovery; Android's own reboot comes back to Linux. adb works from Android, so flashing is done
from there. To undo: `scripts/flash-boot.sh images/boot-fast/boot-slub-off.img` and
`scripts/flash-recovery.sh images/linux/recovery-linux.img`.

```
# the old layout (Linux in recovery) for reference:
scripts/flash-recovery.sh images/linux/recovery-linux.img
adb reboot recovery
```

On the watch: a root shell on the USB serial port (`scripts/watch-shell.ps1`), SSH keys in
`/root/.ssh/authorized_keys` (no password logins). Hold the upper button (the lower one is power) for 3 s in the UI to go
back to Android. Back up Linux with `adb pull /data/linux/rootfs.img` (it holds saved Wi-Fi
passwords: keep it private); delete `/data/linux/rootfs.map` (from Android) to stop booting into
it. Without an installed rootfs, Linux runs from its initramfs and returns to Android after 120 s.

**Grown-up mode:** hold both buttons for 3 s, then answer the sum. Wi-Fi (scan, password
keyboard, forget), a terminal, battery/system status and brightness, Desktop (Sxmo on sway),
back to kid mode, reboot to Android. The screen turning off always returns to kid mode.
`wifi-add "SSID" "password"` in a shell does the same as the Wi-Fi screen. In the desktop (Sxmo),
the upper button opens the menu and touch selects; power (or idle) ends it and returns to the
pups. Sxmo's own Wi-Fi and modem menus do nothing here (no NetworkManager or ModemManager): use
grown-up mode for Wi-Fi.

In a shell: `vibrate 500` (motor), `aplay file.wav` / `arecord` (speaker, mic),
`/sys/bus/iio/devices/*/in_accel_*_raw` (accelerometer), `rc-service bluetooth start` then
`bluetoothctl` (Bluetooth: scanning works, pairing not tried yet; it needs this watch's BT NVRAM,
`port/firmware/README.txt`). The modem's AT port is `/dev/wwan0at0`
(it starts with the radio off, `+CFUN: 0`; calls, SMS and data need a SIM and are not done yet).

**Your own pictures** (personal use; they stay on the watch, never in this repo): put
`pup0`..`pup5` (png/jpg), optionally `home` (background) and `names.txt` in a folder, then
`scripts/theme-push.sh FOLDER` with the watch in Android, and boot Linux. Cut-outs (transparent
PNGs) keep their shape; photos are cropped to a circle. `scripts/theme-push.sh --remove` restores
the built-in art.

![Pup Rescue HQ](docs/img/pupui-home.png)

Undo one package: `adb shell pm enable <package>` (or Settings > Apps > Disabled).
Undo animations: `adb shell settings put global window_animation_scale 1` (same for
`transition_animation_scale`, `animator_duration_scale`).

## Gotchas on this build

- Do not `pm uninstall --user 0` system apps. Nougat has no `install-existing`, and the
  odexed APKs can't be reinstalled. The only undo is deleting the `inst="false"` line from
  `/data/system/users/0/package-restrictions.xml` with the framework stopped (`stop`/`start`).
- Never remove `com.wiite.freezeapp`. The vendor framework checks tethering permissions as
  this package; without it system_server crashes (`SecurityException ... WRITE_SETTINGS`).
- Wait ~15 s after `pm disable-user` before rebooting. PackageManager writes its state lazily.
- Load average sits around 17-20 even when idle. That is 17 MTK kernel threads in D state,
  not CPU use. Check `/proc/stat` deltas instead.
- `com.sprots.wiiteer.fitness` (WearFitnessGPS) links GMS Location/Maps; with GMS off its
  route map will be empty.
