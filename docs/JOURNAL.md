# Rufus Cuff lab notebook

Dated notes, newest last. Hardware: Rufus Cuff (Wiite `C10_DW_480X640_TFT` board, MediaTek
MT6739WA, MT6357 PMIC, 3 GB RAM, 32 GB eMMC, ST7701S 480x640 DSI panel). Plans and current
state live in [PORT-PLAN.md](../PORT-PLAN.md); this file is the history.

## 2026-10-27

### Android: debloat and speed-up

- ADB is root out of the box (`ro.secure=0`, `eng` boot image). Partition backup of everything
  except `userdata`/`cache` taken first (local + NAS, checksummed). `nvram`/`nvdata`/`proinfo`
  hold the IMEI and RF calibration: not in any stock firmware, keep them safe.
- Disabled 21 packages (vendor bloat incl. the Adups FOTA client, MTK logger, Baidu location,
  Wiite app store/task killer/IMEI writer, and all of Google). `scripts/debloat.sh` reapplies or
  undoes it.
- Mistake: `pm uninstall --user 0` on `com.wiite.freezeapp` crashed system_server; the vendor
  framework checks tethering permissions as that package. Nougat has no `install-existing`;
  recovered by editing `/data/system/users/0/package-restrictions.xml` with the framework stopped.
  Lesson: use `pm disable-user` only. PackageManager writes its state lazily: wait ~15 s before
  rebooting after disabling.
- `boot` cmdline got `slub_debug=-` (overrides LK's `slub_debug=OFZPU`): boot 64.8 -> 56.1 s,
  slab 112 -> 68 MB, app cold starts ~17-20% faster.
- No Wiite IMEI/SIM uploader (`watch.wiiteer.com`, as found on the DM100) in this firmware's
  compiled code.

### Research and plan

- No mainline/postmarketOS port for MT6739 exists for this watch; itsvic-dev's AGM M7 work
  (Linux 7.2, MT6739, AI-written, LineageOS + NixOS) is the base. Wiite's own 4.4.22 tree
  (C7S/C8) exists but lacks this board's panel/touch drivers.
- Installed firmware: `system` = TICWRIS MAX V1.2; `boot`/`recovery`/`lk` = an unsigned RC088
  *eng* build that is not on the NAS; `logo` = RC088.

### Phase 0/1: safety net, LK

- SP Flash Tool v5.2020 reaches the preloader and reads eMMC byte-identical to the adb backup
  (flaky: retry with a small partition first). mtkclient through usbipd/WSL fails (the preloader
  times out before forwarding finishes).
- `adb reboot recovery` boots `recovery` once; the next reset returns to Android. Linux lives in
  `recovery`, Android in `boot` stays untouched.
- LK does not verify kernel/ramdisk contents (a recompressed ramdisk and a one-byte kernel change
  both boot), although it reports `flash.locked=1` / `verifiedbootstate=green`.

### Phase 2: first mainline boot

Each of these cost a debugging round:
- **pstore layout:** MTK 4.4 ramoops has two console zones; the real console is `0x5445f000`
  (`console-ramoops`), not the mainline-computed `0x5449f000` (`console-ramoops-2`). Found by
  scanning RAM for persistent_ram headers. For a while working boots looked like failures.
- **LK refuses a DT that reserves the framebuffer region** (`0x7fb10000`); it reserves it itself.
  Found by bisecting with tiny 32/64-bit probe "kernels" that paint the screen and write a marker
  to pstore (`port/probe/`), and grafting stock DT nodes (`scripts/dtb_graft.py`).
- **LK's 64-bit DTB lookup** scans the last 512 KiB of the kernel blob backwards for the FDT magic
  (disassembled at `0x56024b6e`): a 64-bit kernel blob must be > ~512 KiB.
- **LK keeps only ~100 chars of the boot.img cmdline**: the real command line is built in
  (`CONFIG_CMDLINE_FORCE`). Without `clk_ignore_unused` the kernel disabled clocks and CPUs stalled.
- Windows needs a plain CDC-ACM gadget (g_serial) to bind a driver.
- Result: Linux 7.2.9, 4 cores at EL2, 3 GB, eMMC, pstore, watchdog, USB serial shell (COM4).

### Phase 3: display

- OVL0 (LK's overlay engine) keeps fetching LK's own images whatever the CPU writes to the layer
  buffers (verified uncached); cause unknown (secure/larb/IOMMU settings tried).
- **Working:** RDMA0 in memory mode scans the framebuffer directly (`MEM_START_ADDR` 0x1400bf00,
  pitch 0x1400b02c = 0x780, `MEM_CON` 0x1400b024 = 0x20 XRGB8888, `GLOBAL_CON` |= bit 1).
  First via `port/tools/disptool` in `/init`, then a kernel driver
  (`port/kernel/drivers/soc/mediatek/mt6739-rdma-scanout.c`, subsys_initcall).
- Panel is portrait 480x640, worn landscape with the buttons on the right: `fbcon=rotate:3`,
  canvas (x, y) -> framebuffer column 479-y, row x. Big console font `TER16x32`.
- Backlight: Awinic AW9364 on GPIO 85, pulse-count dimming (16 steps), own driver
  `port/kernel/drivers/video/backlight/aw9364-backlight.c`.

### Phase 3: input

- Touch: Hynitron, but at I2C `0x38` (stock DT says `0x5d`) with a FocalTech FT5x06 register map
  -> mainline `edt-ft5x06` (`focaltech,ft6236`). INT GPIO 6, reset GPIO 119, supply `vldo28`.
  Raw coordinates are panel-native; `touchscreen-inverted-x` + `touchscreen-swapped-x-y` give the
  wearer's 640x480. Measured corners: TL (426,58), TR (426,622), BR (14,626), BL (28,48).
- Second button: keypad matrix row 0 col 0 = `KEY_BACK`. Power: `mtk-pmic-keys` = `KEY_POWER`.
- Safety: `mtk-pmic-keys` rewrites the PMIC long-press reset config (`TOP_RST_MISC` 0x14c) at probe
  and *disables* it if the DT properties are missing. Stock value 0x221 -> `long-press-mode = <1>`,
  `power-off-time-sec = <0>`; reads 0x221 again in Android afterwards.
- Testing lesson: the magnetic USB cable drops when the watch is lifted; log input events on the
  watch (`evtest` writes to a file and `/dev/kmsg`, which survives into Android via pstore).

### Phase 3: battery, Wi-Fi

- Battery (read-only): MT6357 AUXADC via `mt6359-auxadc`; `bat_adc` raw x 0.1648 = mV (4080 mV
  while Android said ~4.0 V). Charging appears to pause under Linux (no charger driver), so Linux
  sessions stay short and the watch idles in Android.
- Wi-Fi: AGM's `mt6739-consys` + staging gen2 driver, built in. Firmware
  (`ROMv2_lm_patch_1_{0,1}_hdr.bin`, `WIFI_RAM_CODE_6755`) and NVRAM
  (`/nvdata/APCFG/APRDEB/WIFI` -> `mt6739-wlan-nvram.bin`, MAC at offset 4) copied from the watch.
  The driver's autostart runs before CONSYS probes (-EPROBE_DEFER, no retry):
  `mt6739_wlan.autostart=0` + `/init` writes `wlan_on=1`. `iw` (Alpine musl build) scans 13 APs on
  2.4 and 5 GHz. Not connected to any network yet.

### Kid UI ("Pup Rescue HQ")

- For two Paw Patrol fans, but with original art and names (no copyrighted characters): six
  rescue pups (Blaze/fire, Bolt/police, Breeze/air, Sprout/recycle, Digger/construction,
  Splash/water), team badges, a lookout tower.
- Static C program (`port/pupui/pupui.c`, ~800 lines) drawing on `/dev/fb0` with evdev input. Art
  and DejaVu glyphs come from `gen_assets.py` (PIL, drawn at 4x and downsampled) as a ~7 MB
  `assets.h`, generated at build time and not checked in. `/init` starts it after the boot logs.
- Home screen with four big tiles, plus:
  - **Pup Pop**: a pup climbs out of one of six holes, tap it before it hides; 40 s rounds that
    speed up; always at least one star.
  - **Badge Match**: memory game, 6 pairs of team badges.
  - **Paint**: 7 colours, eraser, "new"; thick strokes with line interpolation.
  - **Pup Parade**: the pups march past; tap one and it jumps, woofs and shows its name.
- Buttons: the red home button (top left) or the second button goes home; power toggles the
  backlight; **holding the second button for 3 s reboots to Android** (the parent exit).
- Bugs found from screenshots and fixed: pups floated above the Pop holes instead of climbing out
  (added a bottom clip to `blit`) and the top row of holes sat in the sky; the Match background
  carried from the blue byte into green (hard blue/green split); the Pop time bar broke into two
  dots near the end; the Parade woof bubble covered the title at the top of the jump.
- Not yet tested by a finger: the whole UI was driven with injected taps. Long-press-to-exit
  and the power button inside pupui still need a real press.
- Known: tearing (one buffer; RDMA scans while pupui redraws). Fix later by flipping RDMA between
  two buffers.

![Home](img/pupui-home.png) ![Pup Pop](img/pupui-pop.png)
![Badge Match](img/pupui-match.png) ![Paint](img/pupui-paint.png)
![Pup Parade](img/pupui-parade.png)

### Test tooling

- `scripts/screenshot.ps1`: dumps `/dev/fb0` on the watch (gzip + base64 over the serial shell)
  and converts it to a PNG in the wearer's view (`scripts/fb2png.py`). The CDC-ACM link drops
  bytes under load (a few percent of transfers while an animation runs), so it retries until the
  gzip CRC checks out. `-Reuse` sends a `/tmp/fb.gz` captured on the watch at the right moment;
  `kill -STOP` the UI around the capture to avoid tearing in the image.
- `port/tools/tap.c` (`tap X Y [X2 Y2]`): evdev nodes accept writes, which inject events, so taps
  and drags can be sent to the touchscreen's node without a finger.
- The webcam over-exposes the backlit screen in a dark room; framebuffer screenshots replaced it
  for UI work.

## 2026-10-27/28 (night): from toy to usable system

### Charging

- The stock DT lists a `sw_charger` at I2C1 `0x6a` (AGM M7 has an HL7005 there), but nothing
  answers on this board (I2C1 scan: only `0x18`). Stock Android charges with the **MT6357's own
  linear charger** (`mt6357_charger` + `mtk_linear_charging`: ICHG 1000 mA, CV 4200 mV, kicked
  every 10 s).
- Why Linux stopped charging: LK leaves the PMIC charger watchdog *enabled* (`CHR_CON9` 0x4c,
  32 s) and the charger off (`CHR_CON1` 0xd84); nothing kicked it. Under Android `CHR_CON9` reads
  0xc (watchdog off).
- Register fields from MediaTek's vendor `mt6357_charger.c` and realme's `mt6357-pulse-charger.h`,
  checked against the stock values while charging (`CHR_CON0` 0xfb6 = 1000 mA, `CHR_TOP_CON5`
  0x205 = 4.2 V, `CHR_TOP_CON3` 0x234e = 4.45 V OV / 7 V VCDT, `CHR_CON1` 0x1f84).
- AGM's `mt6357-charger.c` (detection + fuel gauge) extended with charge control
  (`port/kernel/drivers/power/supply/mt6357-charger.c`, DT `rufus,charge-control`): CV 4.2 V
  (hard cap), 550 mA (stock: 1000), watchdog on and kicked every 10 s poll, termination at
  < 150 mA for 30 s near CV, recharge below 4.10 V, 0-45 C with 3 C hysteresis, software OV cut
  at CV + 50 mV, 8 h limit per plug-in, settings read back before enabling, off at
  suspend/shutdown/unbind (charger handed back as stock leaves it).
- Reviewed by an independent agent before it ran; its findings (shutdown race with the plug
  IRQ, IRQ writing state without the lock, no suspend handling, no software OV, no time limit,
  temperature hysteresis) were fixed.
- Battery node: 2729 mAh and an OCV table from the stock gauge profile (`bat_gm30`
  `battery0_profile_t1`).
- Measured: plugged in, before -480 mA (discharging); with the driver +68 mA at 4.18 V (system
  ~490 mA + battery). Termination after 3 polls, recharge at 4098 mV. **Not yet verified:** that
  the PMIC watchdog really stops charging when kicks stop (`kick_wdt=0` test was pre-empted by
  termination at full battery; repeat at lower charge).

### RTC and clock

- RTC driver (writes nothing at probe), `RTC_HCTOSYS`, no `RTC_SYSTOHC`: Linux never writes it.
- It keeps good time but Linux reads the date 60 years minus a day ahead (2086-10-26 for
  2026-10-27; AGM saw 2069-12-31). Like stock Android, keep an offset: system time = RTC +
  `/var/lib/rufus/rtc-offset` (-1893369600 s, measured), refreshed at shutdown once chrony has
  synced.

### Root filesystem in Android's /data

- `/data` is unencrypted ext4 (21 GB free). The rootfs is an ext4 image file
  `/data/linux/rootfs.img` (1 GiB, Alpine 3.24.3 + OpenRC), installed from Android with
  `scripts/install-rootfs.sh`, which also stores the file's extents as Android's ext4 reports them
  (`rootfs.map`, `port/tools/mapfile.c`, FIEMAP).
- Linux's initramfs sets userdata read-only, mounts it `ro,noload`, maps the file again and only
  continues if the extents equal Android's map; then unmounts, creates a device-mapper linear
  device over exactly those blocks (DM ioctls, no dmsetup), checks the ext4 magic and label
  `rufus-root`, mounts it and `switch_root`s. Any failure: the old initramfs mode.
  Android's filesystem is never written; the image is a normal file there (`adb pull` = backup).
- Bug on the way: the build script stripped CRLF from everything in `/usr/local/bin`, including
  the static binaries, which then crashed in glibc's start-up (a GOT entry read 0x4001 instead of
  0x4001d8: one 0x0d byte gone). md5 on the watch matched the build tree, which cleared the
  device-mapper path; the bytes at the GOT offset showed the shift.
- Services: Wi-Fi power-up, wpa_supplicant (`wifi-add "SSID" "password"`), DHCP, chrony, dropbear
  (keys only), the kid UI (supervised), low-battery guard (reboots to Android at 3 % or
  3.45 V when unplugged; Android then shuts down properly), optional return-to-Android timer
  (`/etc/rufus.conf`). A root shell stays on the USB serial port.

### Power

- PMIC power-off (`mediatek,mt6357-pwrc`) in the DT: PSCI SYSTEM_OFF hangs on this ATF (AGM),
  which on a sealed watch would mean stuck on. In the kernel, not used by the UI, not tested.
- CPU idle (PSCI `cpu-off`, `cluster-off` from `mt6739.dtsi`) works. Kid UI turns the screen off
  after 60 s without input; a tap or the second button wakes it.
- Battery draw (charger idle, system idle): 257 mA screen on before CPU idle, 238 mA with it,
  190 mA with the screen off; Wi-Fi up or down makes no difference. Next suspects:
  `clk_ignore_unused`/`regulator_ignore_unused` (everything stays on), no cpufreq, no suspend.

## 2026-10-28 (day): grown-up mode, own pictures, Sxmo, the rest of the hardware

### Grown-up mode (`port/pupui/grownup.c`)

- Way in: hold both buttons 3 s, then a times-table sum (3 tries). The second button alone for
  3 s still reboots to Android. Power now toggles the screen on release, so the combo does not.
- Wi-Fi: talks to wpa_supplicant's control socket directly (no shell, so SSIDs and passwords need
  no quoting; SSIDs are set as hex). Scan list (strongest first, one row per SSID), password
  screen with an on-screen keyboard (letters, two symbol pages, Show/Hide), Connect/Forget for
  saved networks. A new network is saved only once it connects; after 25 s it is removed again
  ("Wrong password?"). `SELECT_NETWORK` disables the others, so they are re-enabled before saving.
- Terminal: `/bin/sh` on a pty, drawn as a VT100 subset (cursor moves, erase, insert/delete, and
  the cursor position report that busybox's line editing asks for), extra keys Esc/Tab/^C/^D/arrows.
- Status (battery, charger, Wi-Fi, memory, storage, uptime, kernel) and a brightness slider.
- The screen turning off (timeout or power) returns to kid mode and ends the shell.
- pupui is now pupui.c + grownup.c + theme.c sharing `ui.h`; gen_assets.py writes assets.c/.h
  and two more fonts (DejaVu Sans for text, Sans Mono for the terminal).
- Tested with injected input (`tap key CODE 0|1` writes to the key's own evdev node). Lesson: a
  write without a trailing SYN_REPORT sits in the reader's buffer and is delivered with the next
  packet; half-written events from failed attempts then replayed and once looked like a 3 s hold
  of the second button (clean reboot to Android).

### Own pictures (`theme.c`, `scripts/make-theme.py`, `scripts/theme-push.sh`)

- Personal-use themes stay on the watch, never in the repo: `theme-push.sh DIR` converts
  `pup0..5`, `home` and `names.txt` (PIL in WSL: photos cropped to a circle, cut-outs kept) to raw
  ARGB files in `/data/linux/theme`. The initramfs copies that folder (at most 16 MiB) while
  /data is mounted read-only and puts it in the rootfs; removing it removes the theme.

### Desktop: Sxmo on sway

- `rufus-desktop` replaces pupui (exec) and runs Sxmo 1.18 as user `rufus`; pupui's supervisor
  restarts the kid UI when it ends. sway uses pixman (no GPU driver) on simpledrm, `transform 90`
  (checked against pupui with `disptool pattern` and the webcam).
- Needed on the way: eudev (libinput ignores devices udev has not tagged), seatd, a 2 GiB image
  (Sxmo pulls in ~800 MB: llvm for mesa, deno for yt-dlp, GTK, NetworkManager, ModemManager,
  pipewire; none of those services are enabled), setuid modes reapplied from apk's database
  (apk under qemu-user cannot set them), util-linux `setsid` (Sxmo's bonsai runs `setsid -f`,
  which busybox lacks, so no button did anything), buttons bound by keycode (xkb gives this power
  key no XF86PowerOff), and the same in Sxmo's sway mode "menu" (power selects, second button
  moves down), which otherwise left both buttons dead while a menu was open.
- Tested by injection: second button opens the menu, moves and selects; power with no menu
  open goes to screen-off and the watch is back in the kid UI within a second.
- Kid safety: `rufus-desktop` ends the session when Sxmo's state becomes `lock` or `screenoff`.
  A Sxmo screen-off hook would need Sxmo's per-version `configversion` header and is silently
  disabled after an update.
- Not a bug: a "white screen" after the desktop was the webcam overexposing in a dark room;
  `disptool pattern` showed the panel fine.

### Mobile functions: scoped (details in PORT-PLAN.md)

- Mobile data works on mainline on the AGM M7 (same SoC; `mt6739-ccci` is already in our kernel
  tree) but needs LK to load the modem, which it does only in a normal boot from `boot`. Under our
  Linux (`recovery`) `ccci,modem_info_v2` is all zeros. That makes U-Boot in `boot` (AGM's way) the
  gate for data, SMS and later calls: a decision for the owner.
- Fitted, from Android: BMA4xy accelerometer, heart-rate sensor, two cameras. The gyro, compass,
  barometer and NFC nodes in the stock DT are empty.

## 2026-10-28 (afternoon): kernel 7.2.13, audio, vibrator, accelerometer, cpufreq, Bluetooth, boot layout

- **Kernel 7.2.13:** kernel.org incremental patches 7.2.9 -> 7.2.13 on AGM's tree (local branch
  `rufus-7.2.13`); one hunk in `generic-adc-battery.c` needed fuzz (AGM added a function after it).
- **Vibrator:** `regulator-haptic` on the VIBR LDO at 2.8 V (stock `vib_vol = 9` = index 9 of the
  MT6357 table). The webcam's mic is not available to desktop programs, and the fuel gauge's current
  only updates every few seconds, so the proof came from the accelerometer: its spread goes from 0.5
  to 5.8 counts while the motor runs.
- **Speaker and mic:** AGM's MT6739 AFE and MT6357 machine driver; the speaker amplifier is enabled
  by GPIO 13 (stock pinctrl `audexamphigh/low`), described as `simple-audio-amplifier`. Proof by a
  loop: the watch's mic records a 1 kHz tone from its own speaker at RMS 1937 against 244 in silence.
  UCM profile and a boot service route the card, so `aplay`/`arecord` work.
- **Accelerometer:** only 0x18 answers on I2C1 (the stock DT's sensor addresses are placeholders):
  BMA423, chip ID 0x13. New IIO driver for raw data; reads about 1.07 g at rest. Android had loaded
  Bosch's feature blob (INTERNAL_STATUS 1); data registers work without it.
- **cpufreq:** AGM's OPP table is at or above this watch's stock DVFS table (Android
  `/proc/cpufreq/MT_CPU_DVFS_LL/cpufreq_oppidx`) at every frequency, so it is safe here; VPROC and
  VSRAM_PROC capped at the vendor limits. Idles at 299 MHz, but the battery draw did not move
  (244/198 mA screen on/off). A first "90 mA" reading was the slow gauge, not a real change.
- **Bluetooth:** `btmt6739.c` carries H4 packets as STP type 0 through AGM's CONSYS driver, which
  got a receive thread while BT is open, a typed send with a lock and a 4-packet send window, header
  checksum checks and skipping of unsolicited WMT events (the chip sent opcode 0x16 before the BT-on
  reply). A reviewer pass caught a double free (hci core frees the skb when `send` fails) and the
  missing window; one of my own fixes (dropping oversized packets) broke Wi-Fi because the
  calibration reply is 630 bytes, so long WMT replies are cut again as before. Result: `hci0` up,
  MT0699, LMP 4.2; scans run but report nothing. Next: the vendor's NVRAM radio/address commands.
- **Boot layout:** stock Android boots completely from `recovery` (only its modem is off), so
  "Linux in `boot`, Android in `recovery`" gives Linux the modem without U-Boot. `reboot recovery`
  from Linux works (RTC boot-mode bits, `mode-recovery` in the DT). `rufus-android` reads LK's boot
  mode from `/chosen/atag,boot` (little-endian: first attempt read the wrong byte and rebooted into
  recovery, which at least proved that path) and both directions now work. Not switched: writing
  `boot` is the owner's call.
- **Power, regulators:** one boot without `regulator_ignore_unused` switched off only `vmodem`
  and `vaux18`; everything still worked and the draw stayed the same (254/211 mA), so it went back
  in. The clocks (`clk_ignore_unused`) are the bigger suspect, but LK-initialised display clocks
  have no owning driver, so dropping it would very likely blank the panel: not tried unattended.

## 2026-10-28 (afternoon, with the owner's go-ahead): Linux in boot, modem first steps

- Layout switched: stock Android into `recovery`, Linux into `boot` (`scripts/flash-boot.sh`
  saves the old partition first; it matched the known slub-off Android image). The initramfs
  fallback and pupui's initramfs reboot now go to recovery when LK started a normal boot, so
  nothing reboot-loops into Linux. Round trip Linux -> Android -> Linux tested.
- LK now hands over the modem: `ccci,modem_info_v2` at 0x8c000000 (AGM's watch: 0x72000000;
  this one has 3 GB), smem 0x8c000000+3 MB, CCB 0x8a000000+12 MB.
- AGM's `mt6739-ccci` (+ `CONFIG_WWAN`) probes, frees 191 MB of unused modem memory, creates
  `wwan0` and `/dev/ccci_fs`. `echo 1 > .../10014000.modem/state`: MD1 released, HS1 done,
  runtime data sent; then it waits in "booting" for its NVRAM, served through `/dev/ccci_fs`.
- Next: `ccci-fsd` (itsvic-dev/ccci-fsd, Rust) with a copy of the modem NVRAM from `nvdata`,
  `protect1`, `protect2` (service `rufus-modem`, written). Building it from GitHub needs the
  owner's permission (the session's permission system blocked running external code).
- `ccci-fsd` built in the rootfs chroot (owner approved building it; `apk add` again returned
  the qemu setuid error count, which broke the `&&` chain until handled like the main step). The
  `rufus-modem` service copies the modem NVRAM once (nvdata `md`/`md_cmn`, protect1, protect2,
  mounted read-only), runs `ccci-fsd` (log `/var/log/ccci-fsd.log`) and starts the modem.
- Result: **HS2 "MD is ready" 8.5 s after boot, `/dev/wwan0at0` appears**, the modem reads ~50
  NVRAM files successfully (one early GetAttributes probe answers -9 = not found, normal). Then,
  untouched, it raises a **fatal exception about 33 s after HS2**, every boot (EX_OVERVIEW at smem
  0x800: type byte 0x20, code 0x2de, no assert file), before answering AT. No unhandled packets
  are logged, and the one DT query it makes (`mediatek,md_product_name_model_id`) is absent in the
  stock DT too. Firmware here is MOLY.LR12A.R2.MP.V5.23.P10 (2019-05-08); the AGM M7 runs V96.6.
  Next: watch what stock Android does with its modem in the first minute (needs Android back in
  `boot` for a while, since it only gets the modem in a normal boot).
- Stock comparison (Android back in `boot` for one boot, logs in `logs/`, not published): stock
  keeps the modem alive without a SIM. Before the start, `ccci_mdinit` reloads the modem type
  (`CCCI_IOC_RELOAD_MD_TYPE 0x5a00001f`, then type 10) and configures three CCB users
  (`ccb_dhl`, `ccb_md_monitor`, `ccb_meta`) through `ccci_ccb_ctrl`. After HS2 the modem sends
  RPC 0x400E (stock answers "unknown operation"), system message 0x110 at +1.2 s (stock answers
  with the TX power mode), `gsm0710muxd` opens `ttyC0` at +1.5 s and the RIL starts sending AT.
- On Linux the modem answers no AT command at all (sent from +0 s after HS2), never sends
  message 0x110, and still dies ~33 s after HS2. Tried and kept, no change: the DT query answered
  "unknown" as stock (was a 0x0f-filled success); VMODEM set to 1.19375 V as the vendor does
  from efuse word 28 bit 27 (LK had left 1.0875 V). The CCCI driver is now a Rufus copy in
  `port/kernel` with these two changes.
- Not yet tried: the modem type (`RELOAD_MD_TYPE`) and the CCB configuration, which stock's
  `ccci_mdinit` sets before starting the modem. Doing that without guessing needs the vendor
  CCCI source (`eccci`, `port_smem.c`); AGM's `ref/lumi-mt6739` is not on this machine.
- Vendor 4.4 CCCI source: `github.com/fukehan/kernel-4.4` (`drivers/misc/mediatek/eccci`,
  `ccci_util`), read from a sparse checkout in WSL `~/ref/vendor-4.4` (not in this repo).
- From it: the SBP_ID runtime feature carries the work-mode (RAT) bitmap of the universal modem
  image; stock sends 0x3b (from `RELOAD_MD_TYPE 0x5a00001f`: G, TD-SCDMA, W, TDD-LTE, FDD-LTE),
  AGM's driver sent 0. Set as stock (kept); no change in behaviour.
- The crash decoded with `fsm/mdee_dumper_v3.h`: EX_OVERVIEW ex_type 0x20 = **NMI_EXCEPTION** on
  VPE 2, fatal content code1 0x20, PCs 0x90c8c765/0x90c8c773. An NMI ~30 s in is the modem's
  watchdog: a modem thread is stuck (hardware it waits for, or an AP handshake). The PCs need the
  modem's symbol database to name the code, which is not public.
- Step 1, vendor power-on (`eccci/mt6739/md_sys1_platform.c`) against AGM's driver: the same
  sequence (clock mode, PMIC, MTCMOS, bus protection, srclkena 0x21, DVFSRC MD srclkena, PLL
  init with the same values, MD watchdog off, boot slave). `vmd1_pmic_setting_on` only sets
  VMODEM (and checks VCORE). Nothing new.
- Step 2, register state 10 s after "ready" (Linux) against stock with its modem up (dumps in
  `logs/`, tools `scripts/watch-pull.ps1`, a 32-bit `disptool` for Android's `/dev/mem`):
  - PMIC: DCXO CW04 (crystal trim CDAC 0x88 against 0xcc) and CW09 differ, VSIM1 is on in stock:
    probably the stock modem's own doing after RF calibration (effects, not causes). Most other
    differences are low-power modes (`*_OP_EN`/`OP_CFG`) and RF LDOs under srclken HW control.
  - DVFSRC is unconfigured under Linux (stock: `dvfsrc_init` values, BASIC_CONTROL 0x23B).
  - **The SPM firmware is not running under Linux** (SPM PCM registers +0x40..+0x7c are zero;
    stock loads `spmfw`). DVFSRC requests (the MD asks for VCORE/DDR levels through it) need it.
    AGM runs without SPM firmware and its modem works, so this may or may not be the cause here.
- Option 1 tested: DVFSRC set exactly as stock's `dvfsrc_init` (BASIC_CONTROL 0x23B) before a
  manual modem start. No change: no AT answer, NMI at +33 s, DVFSRC level never moved (no SPM
  firmware to act on requests). Not pursued further (loading the SPM firmware was only worth it
  if this helped).

## 2026-10-28 (evening): picture turned 180°

- The owner, holding the watch as worn (buttons on the right): the picture is upside down and
  taps miss. Taps recorded on `/dev/input/event3` came in at (27,60) and (615,437), the canvas's
  top-left and bottom-right corners, so the touch mapping in the DT was right all along and only
  the drawing was 180° off.
- Turned: pupui now maps canvas (x, y) to framebuffer row 639-x, column y; `fbcon=rotate:1`;
  sway `transform 270`; `scripts/fb2png.py` rotates the other way. The DT touch properties are
  unchanged.
- Buttons, as worn: the upper one is `KEY_BACK` (pupui home, Sxmo menu), the lower one power.
  Earlier notes say "second button" for `KEY_BACK`.
- Desktop "touch does nothing": touches reach the kernel and sway has the device enabled
  (calibration identity, output 640x480 logical). Sxmo's bare desktop only takes edge swipes;
  taps act in menus and apps. Sxmo locks after 2 min idle, which `rufus-desktop` treats as
  screen-off and ends the session.
- Correction: taps in Sxmo's menu landed in the wrong place. wlroots rotates touch by the
  output transform when the device is mapped to the output (Sxmo maps it), on top of the DT's
  own turn to landscape. Fixed in `rufus.conf` with `calibration_matrix 0 1 0 -1 0 1` (libinput
  hands sway panel-native coordinates); confirmed accurate by the owner. Menus: `BEMENU_OPTS`
  in the device profile, 48 px rows with a 22 pt font (Sxmo's 20 px rows were too small to hit).

## 2026-10-28 (evening): the modem stays up and answers AT

- The vendor driver offers `CCB_SHARE_MEMORY` (15) and `DHL_RAW_SHARE_MEMORY` (16) as MUST for
  `MD_GENERATION` 6293, which MT6739 is (`eccci/mt6739/ccci_config.h`); AGM's driver offered
  neither. Offered alone, the MD now asserts 3 s after HS1, before HS2:
  `ASSERT_FAIL` (0x50) in `mcu/common/service/hif/cccisrv/ccci_shm_bm/src/ccci_shm_bm.c` line
  734. It validates the CCB control area, which stock's `ccci_mdinit` fills before the handshake.
- Read back from stock with its modem up (Android in `boot` for one boot, `/dev/mem` at
  0x8c018000, dumps in `logs/`, not published): 40 records of 32 bytes, one per buffer and
  direction of the vendor's 20 `ccb_configs`: 0xddbbccaa, four ring indices, page size, buffer
  size, end guard. The end guard is 0x1100ffee for DHL (stock's `emdlogger` reads those) and
  0xeeff0011 for MD monitor and META (nothing reads them). Stock's runtime data matches ours:
  control 0x40018000+0x1000, data 0x4e000000+0x17c000 (the vendor's buffer total; AGM sent
  2 MiB), DHL raw 0x4e200000+0xa00000.
- The driver now writes that table (all records 0xeeff0011, no log consumer on Linux) when it
  clears shared memory, and offers both features. Result: HS2 at 6.4 s, **no exception** (still
  `ready` well past the old +33 s), and `/dev/wwan0at0` answers: `AT` OK with `+EIND: 64` and
  `+EIND: 128`, `+CFUN: 0`, `+CPIN?` "SIM busy" (no SIM fitted), `+CSQ: 99,99`, `+COPS: 0`.
- So the NMI at +33 s was the modem's watchdog catching a task stuck on the missing CCB setup.
  The SPM firmware (the other candidate) was not needed. `RELOAD_MD_TYPE` only changes the
  AP-side load type in the vendor driver (`fsm/ccci_fsm_ioctl.c`), so it was not ported.
