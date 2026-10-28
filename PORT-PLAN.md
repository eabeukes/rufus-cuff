# Mainline Linux port plan: Rufus Cuff (MT6739)

Based on itsvic-dev's AGM M7 work: [linux-mt6739](https://github.com/itsvic-dev/linux-mt6739)
(Linux 7.2), [nixos-agm-m7](https://github.com/itsvic-dev/nixos-agm-m7) (docs, scripts),
[u-boot-mt6739](https://github.com/itsvic-dev/u-boot-mt6739). Local copies of their docs are in `ref/agm-m7/`.

## Goal and ground rules

The goal is a toy-grade Linux watch for a kid: screen, touch, buttons, a few simple apps.
Anything beyond that is a bonus.

**The watch must stay a working Android toy at every step.** If the port stalls at any
phase, the watch goes back to debloated stock Android and nothing is lost. So:

1. Stock Android stays in `boot` until the very last (optional) phase. Linux lives in
   `recovery` and starts with `adb reboot recovery`. LK boots `recovery` once, so any hang
   or crash ends with a reset back into Android.
2. The Linux root filesystem is a file on `/data` (`/data/linux.img`, loop-mounted). No repartitioning.
3. Restore images come from `backup-20261027/`, not from the NAS (see "What's installed").

## What's installed (checked 2026-10-27)

| Partition | Source | Notes |
|---|---|---|
| `system` | TICWRIS MAX V1.2 (`flash_ticwris_.../runme.cmd` flashes only `system`) | `ro.build.display.id=TICWRIS_MAX_V1.2_20200107` |
| `logo` | RC088 | byte-identical to `RC088_pro_V0.4_20201020_P.zip` |
| `boot`, `recovery` | an **RC088 `eng` build that is not on the NAS** | unsigned, `buildvariant=eng`, `ro.secure=0`, recovery reports `product:RC088`. Every NAS `boot.img`/`recovery.img` is a signed `user` build |
| `lk` | not on the NAS | probably the same RC088 eng drop |

**LK boots unsigned `boot` and `recovery` while reporting `flash.locked=1` and
`verifiedbootstate=green`.** Confirmed by booting the installed unsigned recovery
(`adb reboot recovery`, then `adb reboot` returns to Android). So custom images should boot
without an unlock. Phase 1 shrinks to one test.

The AGM M7 code is mostly AI-written and barely reviewed (their warning). That's fine for a
toy, but expect to debug it, and none of it can be upstreamed to postmarketOS.

## Watch vs AGM M7

| | AGM M7 | Rufus Cuff | Impact |
|---|---|---|---|
| SoC / PMIC | MT6739 / MT6357 | MT6739 / MT6357 | CPU, clocks, eMMC, USB, audio codec, charger and fuel gauge code reuses directly |
| Stock kernel | 32-bit 4.4.95, `bootopt=64S3,32S1,32S1` | 32-bit 4.4.22, same bootopt | Same trick should give a 64-bit kernel (`64S3,32N2,64N2`) |
| Stock Android | 8.1, Treble | 7.1.1, no Treble | No LineageOS GSI as a side OS; keep stock Android |
| ODM DTBO overlay | yes (LK asserts if it fails) | no `odmdtbo` partition | One less trap |
| Panel | GC9306 240x320, DBI command mode | ST7701S 480x640, **DSI video mode** (`lk_st7701S_V2_480X640_dsi_vdo`) | Video mode scans LK's framebuffer continuously, so `simple-framebuffer` should work with no display driver |
| Touch | Telink TLSC6x via `edt-ft5x06` | Hynitron "CTS0XX" at I2C0 `0x5d` (`hyn_ts`) | Try mainline `hynitron-cst816x`/`hynitron_cstxxx`, then `edt-ft5x06` |
| Keys | 22-key keypad | `mtk-kpd` (exact physical buttons TBD) | Same driver |
| `userdata` | dm-crypt + f2fs | **unencrypted ext4** | Root image on `/data` is trivial |
| Storage for root | microSD | no card slot found (mmc1 host is empty) | Root on `/data` |
| Wi-Fi | CONSYS + vendor gen2 driver in staging | `CONSYS_MT6739`, same firmware names | Should port with firmware and NVRAM from our partitions |
| Extras | - | heart rate (`wiite-hrs`), BMA4xx accel, Nordic co-processor (`wiite_nrf`), NFC, cameras | Out of scope |

Unknowns: the stock DT's `/chosen/atag,videolfb-lcmname` says `rm67160_400x400_dsi_cmd_oled`
while LK and Android say ST7701S 480x640. Probably a leftover string; check in Phase 2.

## Phase 0: safety net (no risk, ~half a day)

- [x] Copy `backup-20261027/` to the NAS (33 files, checksums verified). `nvram`, `nvdata`, `nvcfg`, `proinfo` are the only copies of the IMEI, MACs and RF calibration.
- [x] Dump the live device tree and memory map: `hw/dt.tar`, `hw/live.dtb`, summary in `hw/NOTES.md` (pstore at `0x54410000`, LK framebuffer at `0x7fb10000`).
- [x] Scan the firmware for the DM100's Wiite uploader (`watch.wiiteer.com`): not present in any compiled app or framework file. Native libraries not scanned.
- [x] Webcam on the watch screen instead of UART: `scripts/cam.sh` (snapshot, or 1 frame/s burst for boots). Camera position still needs adjusting.
- [x] **Unbrick path proven (2026-10-27): SP Flash Tool v5.2020 via the preloader.** MediaTek VCOM drivers (`usbvcom.inf`, `usbvcom_brom.inf`) from the NAS, console mode with `tools/rb-test.xml` (`readback-withoutscatter`, read-only). Watch fully off, then plug in with no buttons: `PreLoader USB VCOM Port`, chip `0x6739`/`0x8A00`, `MTK_AllInOne_DA_v3.3001` loads, GPT reads, and `loader_ext1`, `para`, `boot_para` came back **byte-identical to the adb backup**. Restore = SP Flash Tool download with the scatter file and the `backup-20261027` images.
  - Flaky: the first two runs (only `boot_para`, high-speed on then off) stalled on the first data packet (`data_mux receive timeout`, `STATUS_UPLOAD_EXCEPTION`). The third run (64 KiB, then 512 KiB, then 1 MiB) worked. Cause unknown. If a restore stalls: unplug, hold power 10 s, retry with a small partition first.
  - Writes not tested on purpose. The preloader and DA path is the same for download.
- [x] mtkclient through usbipd + WSL: **does not work**. The preloader drops off USB about 2.5 s after enumeration, before the usbipd attach and mtkclient handshake finish. usbipd-win is installed with AutoBind policies for `0e8d:0003`/`0e8d:2000`, if we want to retry natively on Windows later.
- [ ] BROM key combo: unknown. Holding the second button while plugging in didn't produce BROM (the watch may not have been fully off). Only needed if the preloader itself breaks, and nothing in this plan writes `preloader`/`mmcblk0boot0`.
- [x] Buttons: two (power + one). Preloader mode needs no button: fully off, then plug in.
- [x] Prove the one-shot recovery: `adb reboot recovery` boots the installed recovery, and the next reboot goes back to Android. **The whole safety plan depends on this.** (Done 2026-10-27.)
- [x] Turn today's debloat into `scripts/debloat.sh` (`undo` reverses it). The list is checked against `packages-disabled.txt`.
- [x] UART: not possible (case stays closed). Phase 2 uses pstore RAM logs plus the webcam.
- [x] Set up the build host: WSL2 Ubuntu 24.04 via `scripts/setup-wsl.sh` (user `builder`): `aarch64-linux-gnu-gcc` 13.3, `pmbootstrap` 3.12.1, AGM M7 trees in `~/src` (`linux-mt6739`, `u-boot-mt6739`, `nixos-agm-m7`).

**Exit:** a tested way to restore any partition, with and without a booting Android.

## Phase 1: prove LK takes our images (~1 hour; unlock only if this fails)

The installed images are already unsigned, so this is very likely a formality.

**Done 2026-10-27. No unlock needed.**

- [x] `scripts/bootimg_cmdline.py` appends to a header-v0 image's cmdline and changes nothing else (it checks that it recomputes the original `id` hash). Both images are named `RC088_pro_V0.3_`.
- [x] Recovery with `slub_debug=- initcall_debug=0 rufus.phase1=1` written to `recovery` (md5 verified), `adb reboot recovery`: **LK booted it**. `adb pull` works in recovery without a shell: `/proc/cmdline` had the marker, and `kmalloc-128` `sanity_checks`/`red_zone`/`poison`/`store_user` were all 0. Original recovery restored afterwards (md5 verified).
- LK's cmdline order: `slub_debug=OFZPU` comes before the header cmdline (ours wins), `initcall_debug=1` comes after it (LK's wins; boot-time cost only).
- [x] Quick win: `boot` now has `slub_debug=-` (13 bytes changed, `images/boot-fast/boot-slub-off.img`). `scripts/bench.sh`, one boot and 3 launches per app:

  | | stock | `slub_debug=-` |
  |---|---|---|
  | boot_completed | 64.8 s | 56.1 s |
  | Slab | 112 MB | 68 MB |
  | Settings / Dialer / Contacts cold start | 803 / 945 / 1140 ms | 670 / 772 / 911 ms |

- Unlock stays unneeded unless a future LK refuses our images.

**Exit:** met. A modified `recovery` boots, and Android boots normally (faster).

## Phase 2 result (2026-10-27): done, USB shell on mainline Linux 7.2.9

`recovery` holds `images/linux/recovery-linux.img` (`scripts/build-port.sh` in WSL,
`scripts/flash-recovery.sh`, `adb reboot recovery`). The USB gadget is g_serial: Windows shows
"USB Serial Device (COM4)", `scripts/watch-shell.ps1` runs commands. 4x A53 at EL2, 3 GB RAM,
eMMC HS200 (all 32 partitions), pstore, watchdog, simpledrm. `/init` reboots to Android after
120 s unless `touch /tmp/stay`; `reboot -f` goes back to Android.

What it took (each one cost a debugging round):

- **pstore layout:** MTK's 4.4 ramoops has two console zones. Console = `0x5445f000`, not the
  mainline-computed `0x5449f000` (that is `console-ramoops-2`). Found by scanning RAM for
  persistent_ram headers (`hw/pstore-region.bin`).
- **LK refuses a DT that reserves the framebuffer.** A `/reserved-memory` node at `0x7fb10000`
  made LK hang before the kernel (it reserves that range itself as `mblock-5-framebuffer`).
  Found by bisecting with probes (`port/probe/`, `scripts/dtb_graft.py`): tiny 32/64-bit
  "kernels" that paint the framebuffer and write a marker to pstore. Also kept: `/led@0..6`
  copied from stock, no `/aliases`, property-only `/chosen` (harmless, unproven whether needed).
- **LK's 64-bit DTB lookup** scans the last 512 KiB of the kernel blob backwards for the FDT
  magic (disassembled at `0x56024b6e`), so a 64-bit kernel blob must be > ~512 KiB.
- **LK keeps only ~100 chars of the boot.img cmdline** (cut ours mid-word), so the command line
  is built in (`CONFIG_CMDLINE` + `CONFIG_CMDLINE_FORCE`); the header carries only `bootopt=`.
  Without `clk_ignore_unused` the kernel disabled clocks and CPUs 1/3 stalled (RCU).
- **Windows needs a plain CDC-ACM gadget** (g_serial); g_cdc's ECM half leaves the whole
  composite device without a driver.
- Phase 1's "LK accepts our images" was proven only for cmdline edits; content edits were
  proven later (recompressed ramdisk, one-byte kernel change both boot). LK does not verify.

## Status (2026-10-28): display, input, battery and charging, clock, rootfs, Wi-Fi, kid UI, grown-up mode, audio, vibrator, accelerometer

| Part | State | How |
|---|---|---|
| Display | working from early boot | `port/kernel/drivers/soc/mediatek/mt6739-rdma-scanout.c` points RDMA0 at the simple-framebuffer (table below); fbcon `rotate:1`, `TER16x32` |
| Backlight | working, 16 steps | AW9364 on GPIO 85, `port/kernel/drivers/video/backlight/aw9364-backlight.c`, `/sys/class/backlight/backlight` |
| Touch | working | `edt-ft5x06` at I2C0 `0x38`, swapped + inverted-x -> wearer's 640x480 |
| Keys | working | upper button `KEY_BACK` (keypad), lower button power `KEY_POWER` (`mtk-pmic-keys`, stock long-press reset kept) |
| Battery | gauge + charging | `mt6357-charger` (AGM) + our charge control of the PMIC linear charger: 4.2 V, 550 mA, termination, 0-45 C, OV and time limits; watchdog stop not yet verified |
| RTC / clock | working | RTC read only, + saved offset (Linux reads the date 60 years ahead); chrony on Wi-Fi |
| Wi-Fi | scans and joins (from grown-up mode, tested by the owner) | `mt6739-consys` + staging gen2, firmware/NVRAM from the watch (`port/firmware/README.txt`); grown-up mode or `wifi-add` |
| Root filesystem | working | Alpine 3.24 + OpenRC in `/data/linux/rootfs.img` (`build-rootfs.sh`, `install-rootfs.sh`), mapped by extents, Android's fs never written |
| Power | CPU idle + cpufreq; PMIC power-off untested | PSCI idle states; `mediatek-cpufreq` 299 MHz-1.27 GHz on VPROC (AGM's OPP table, at or above this watch's stock DVFS voltages), schedutil; `mt6357-pwrc`; screen off after 60 s in the UI; low battery -> reboot to Android. Draw unchanged by cpufreq: 244 mA screen on, 198 mA off (60 s averages) |
| Kernel | Linux 7.2.13 | AGM's 7.2.9 tree + stable 7.2.10-13 (branch `rufus-7.2.13` in WSL `~/src/linux-mt6739`) |
| Audio | speaker + mic working | AGM's AFE/MT6357 machine driver, speaker amp = `simple-audio-amplifier` on GPIO 13; UCM profile `Rufus_Cuff`, `rufus-audio` service; loop test: mic hears a 1 kHz speaker tone 18 dB over silence |
| Vibrator | working | `regulator-haptic` on VIBR (2.8 V, stock `vib_vol`); `vibrate MS`; accelerometer spread 0.5 -> 5.8 counts while it runs |
| Accelerometer | working | BMA423 at I2C1 0x18 (chip ID 0x13), our IIO driver `port/kernel/drivers/iio/accel/bma423.c`: raw x/y/z, +-4 g, temperature |
| Bluetooth | scans, pairs (bonded with an iPhone over BR/EDR, encrypted); no profile used yet | `btmt6739.c` HCI over STP type 0 through the CONSYS driver; at each power-on it replays stock's CONSYS init from the BT NVRAM (radio, TX power, sleep, address) and fixes the firmware's supported-commands table (LE octets reported as zero); MT0699, LMP 4.2 |
| Back to Android | working in both layouts | `rufus-android`: LK boot mode from `/chosen/atag,boot` (little-endian u32 size/tag/mode) picks a plain reboot or `reboot recovery` (RTC boot-mode bits, `rufus-reboot`); both tested |
| Kid UI | working | `port/pupui/` ("Pup Rescue HQ"), OpenRC service; clock in the tower, battery with charging bolt; own pictures via `theme-push.sh` (`theme.c`) |
| Grown-up mode | working (tested on the watch) | both buttons 3 s + a sum (`grownup.c`): Wi-Fi scan/connect/forget over wpa_supplicant's control socket, terminal (pty, VT100 subset), status, brightness; screen off -> kid mode |
| Desktop | working (tested on the watch; touch calibrated for sway) | Sxmo 1.18 on sway (pixman), user `rufus`, `rufus-desktop`; buttons by keycode into Sxmo's bonsai (upper button = volume-up: menu; in menus Down/Return); lock or screen off -> back to the kid UI |

RDMA0 in memory mode scans the simpledrm buffer directly, bypassing LK's OVL0:

| Register | Value | |
|---|---|---|
| OVL0 `SRC_CON` `0x1400a02c` | 0 | LK's layers off |
| RDMA0 `MEM_START_ADDR` `0x1400bf00` | `0x7fd68000` | simple-framebuffer `reg` (3rd LK vram frame) |
| RDMA0 `MEM_SRC_PITCH` `0x1400b02c` | `0x780` | |
| RDMA0 `MEM_CON` `0x1400b024` | `0x20` | XRGB8888 (`0x30` treats alpha as red, `0x1x0` swaps R/B) |
| RDMA0 `GLOBAL_CON` `0x1400b010` | `0x103` | was 0x101 (direct link); bit 1 = memory mode |

Orientation: the panel is portrait 480x640, worn landscape with the buttons on the right, so
canvas (x, y) is framebuffer row 639-x, column y (`fbcon=rotate:1`; until 2026-10-28 this was
turned 180°, see `docs/JOURNAL.md`).

Still unexplained (no longer blocking): OVL0 layers keep fetching LK's original images even when
the CPU-visible DRAM at the layer address is rewritten (verified uncached). RDMA reading the same
address sees the new data, so it is OVL-specific (secure/larb settings tried, no effect).
Register dump: `hw/disp-regs-1.txt`.

Open: verify the charger watchdog stops charging (repeat the `kick_wdt=0` test below ~90 %), Wi-Fi
association (needs a network), power-off test (needs someone to press power afterwards), cpufreq
(describe the CPU supply), suspend, screen tearing (single buffer; flip RDMA between two buffers).

### Mobile functions and the rest of the hardware (scoped 2026-10-28)

What is fitted (Android, 2026-10-28: bound I2C drivers and `dumpsys sensorservice`): Bosch
BMA4xy accelerometer (I2C1 `0x4c`, `bma4xy_acc`; step counter, tilt and wake gesture are its
features), heart-rate sensor (I2C1 `0x33`, vendor `heartrate` driver; also reports light), two
cameras. The stock DT's gyro, compass, barometer and NFC nodes have nothing behind them. Modem
firmware `MOLY.LR12A.R2.MP.V5.23.P10`.

| Function | State elsewhere | What it takes here |
|---|---|---|
| Mobile data (LTE) | The AGM M7 (same SoC, modem branch LR12A.R2 V96.6) runs it on mainline: `mt6739-ccci`, `ccci-fsd` NVRAM service, a ModemManager plugin (`~/src/nixos-agm-m7`). | **Modem up and answering AT** (2026-10-28): Linux in `boot` so LK loads it, `ccci-fsd` with this watch's NVRAM, and the CCB control table plus the CCB/DHL raw features the vendor offers on MT6739 (without them the MD watchdog fired 33 s after HS2). `/dev/wwan0at0`: `+CFUN: 0`, no SIM fitted. Next: a SIM, `AT+CFUN=1`, registration, then data (ccmni) and ModemManager. |
| SMS | Not done on mainline. | After data: AT `+CMGS`/`+CMGL` on the CCCI AT port; ModemManager does SMS on AT modems. |
| Voice calls | Not done on mainline MT6739. | Modem PCM path plus audio. Large. |
| Bluetooth | Scanning and pairing work 2026-10-28 (`btmt6739.c`, stock's init replayed, firmware command table fixed). | A profile to use: audio (A2DP, needs PipeWire/BlueALSA), or notifications from the phone. |
| GPS | Not done. STP channel 2. | The position engine (`mnld`) is closed Android userspace; the chip gives raw data. Unlikely. |
| Accelerometer | Done 2026-10-28: BMA423, raw IIO driver. | Step counter/tilt need Bosch's feature config blob uploaded. |
| Heart rate | Vendor driver + closed algorithm. | Unlikely. |
| Audio | Done 2026-10-28: speaker and mic. | PipeWire/Sxmo integration; UI sounds. |
| Vibrator | Done 2026-10-28: `regulator-haptic`, `vibrate`. | Use it in the UI. |

### Boot layout: getting Linux the modem (switched to layout 1 on 2026-10-28)

Now: Linux in `boot` (`scripts/flash-boot.sh images/linux/boot-linux.img`, a copy of
`recovery-linux.img`), stock Android in `recovery` (`images/linux/android-in-recovery.img`). The
old `boot` was saved as `images/boot-before-20261028-115056.img`. Round trip tested: Linux ->
`rufus-android` -> Android (bootmode recovery) -> Android's reboot -> Linux (LK mode 0, modem
loaded: `ccci,modem_info_v2` at 0x8c000000, 28 tags). The initramfs fallback and pupui's
initramfs reboot also go to recovery when LK started a normal boot.

LK loads the modem only when it boots `boot`. Two ways to give Linux a normal boot:

1. **Swap the partitions** (recommended first step): Linux `boot.img` in `boot`, the stock
   Android `boot.img` in `recovery`. Tested: stock Android 7 boots completely from `recovery`
   (`ro.bootmode=recovery`, watch face, adb; only its modem stays off). Linux's way back to
   Android is then `reboot recovery`: the RTC driver sets LK's boot-mode bit (`mode-recovery`
   in the DT, `CONFIG_REBOOT_MODE`), and `rufus-android` (used by the UI's 3 s hold, the
   grown-up menu and the low-battery guard) picks plain reboot or recovery from LK's boot mode
   (`/chosen/atag,boot`). Same images and flashing tools as today; undo = flash them back.
   Open: LK's offline charging also boots `boot`, so it would start Linux (which charges).
2. **U-Boot in `boot`** (AGM's way): a menu, kernels from files, fastboot. AGM's U-Boot boots
   LineageOS on their mainline kernel; chain-loading this watch's stock 32-bit vendor kernel
   (which expects LK's atags) is unproven. Worth it later, on top of layout 1.

Testing tools: `scripts/screenshot.ps1` pulls `/dev/fb0` as a PNG over the serial shell (in
md5-checked 16 KiB chunks); `tap X Y [X2 Y2]` on the watch injects taps/drags into the
touchscreen's evdev node, `tap key CODE 0|1` presses or releases a key (116 power, 158 back).

## Phase 3 notes: display (historical)

- OVL0 `0x1400a000`: `SRC_CON` 0x9 (layers 0 and 3), `L0_ADDR` `0x7fc3c000` (2nd vram frame),
  `L3_ADDR` `0x7fd68000` (3rd frame, LK text overlay, per-pixel alpha), ROI 640x480.
  RDMA0 `GLOBAL_CON` 0x101 (direct link). DSI0 mode 1 (video).
- Painting `0x7fc3c000` with `devmem` showed a white stripe along the watch's right edge
  (screen top = side with the buttons): the panel is 480x640 portrait, used landscape;
  Android runs ROTATION_90, so `fbcon=rotate:1` (needs `CONFIG_FRAMEBUFFER_CONSOLE_ROTATION`).
- Open problem, narrowed down 2026-10-27 (Phase 3 session 1):
  - **The pipe runs.** RDMA0/DSI0/MUTEX0 interrupt status bits come back within 0.2 s of
    clearing (OVL `INTSTA` is at `0x008`; `0x004` is `INTEN`). RDMA `INTSTA` 0x26 likely
    includes an underflow bit.
  - **OVL control writes apply immediately.** `ROI_BGCLR` (0x028) red/blue/black and
    `SRC_CON` (0x02c) changes show on screen at once. Layer control is ARGB8888 with
    per-pixel alpha (`L0_CON` 0x27FF; bit 8 = alpha enable, `CLRFMT` bits 15:12 = 2).
    Linux's `0xFF000000` black reads as transparent with alpha on (byte order), so clear
    bit 8 (0x26FF) or set `BYTE_SWAP` (bit 24, as AGM does).
  - **LK's images:** layer 0 at `0x7fc3c000` = charging icon on a transparent background;
    layer 3 at `0x7fd68000` = the small status text. The "grey" was the background colour.
  - **Unsolved:** writing `L0_ADDR` (0xf40) switches between those two LK images, but CPU
    writes to the buffers never show. `0x7fd68000` read back uncached was all white (simpledrm
    or `devmem`) while the layer kept showing LK's original text. Ruled out: genpd (Linux
    only manages `mfg`/`conn`), SMI larb0 MMU (`0xfc0` = 0; `0x380+` per-port values are
    probably outstanding limits, restored), low address bits (`…003`).
  - **Next:** build a small static tool into the initramfs that mmaps `/dev/mem` (fast fills,
    register dumps) instead of per-word busybox `devmem`; dump the whole OVL block
    (0x000-0x1ff and 0xf00-0xfff, including per-layer RDMA control at 0xc0+) and the
    stock DT's `disp_ovl0`/`smi_larb0`/`m4u` nodes; try RDMA0 memory mode (read our buffer
    directly, bypassing OVL) to tell an OVL fetch problem from a memory-path one.
- `watch-shell.ps1` drops a `reboot -f` sent after other commands; send it alone and keep the
  port open ~3 s (see the transcript of 2026-10-27).

## Phase 2: first mainline boot, blind (the hard part, 2-5 days)

- [ ] Build `linux-mt6739` (branch `linux-7.2`) for arm64 with the AGM defconfig.
- [ ] Write `mt6739-rufus-cuff.dts` on top of their `mt6739.dtsi`:
  - memory and reserved regions from our stock DT (`/proc/device-tree` dump), LK fills `/chosen`
  - `simple-framebuffer` at LK's framebuffer (`atag,videolfb-fb_base`), 480x640
  - UART0, eMMC (`msdc@11230000`), USB (`usb0@11200000`), keypad, watchdog
  - pstore/ramoops region (take the stock address, as AGM did with `earlycon=pstoreram`)
- [ ] Initramfs: busybox, USB CDC gadget (serial + ECM network), telnetd. Copy AGM's.
- [ ] `boot.img` header v0: `Image.gz` + DTB appended, kernel address 512 KiB aligned, header cmdline with `bootopt=64S3,32N2,64N2`, a non-empty ramdisk (LK refuses empty ones).
- [ ] `dd` it to `recovery`, `adb reboot recovery`. After the reset, read `/sys/fs/pstore` from Android.
- If LK refuses the 64-bit image: fall back to a 32-bit ARM build of the same tree (more work, but the LK path is known to work).

**Exit:** a shell over USB networking from mainline Linux. Then "Linux on the watch" is done; everything after it is features.

## Phase 3: screen, touch, buttons (2-5 days)

- [x] Display: `simple-framebuffer` + fbcon first (RDMA0 scanout, see Phase 3 status). If the panel blanks or tears, look at the stock `dsi0@14012000` setup or port mainline `panel-sitronix-st7701` with MT6739 DSI (the bigger job).
- [x] Backlight: stock DT has `aw_bl@0` and `wiite_bl@0` (custom), plus `disp_pwm0@1100e000`. Find which GPIO/PWM LK leaves on. AGM M7 used `disp_pwm` + GPIO85.
- [x] Touch at I2C0 `0x5d` (really `0x38`, `edt-ft5x06`): read the chip ID, then try `hynitron-cst816x`, `hynitron_cstxxx`, `edt-ft5x06` in that order.
- [x] Keys: `mtk_keypad` with our keymap; power key from the MT6357.
- [ ] Battery and charging: reuse `mt6357-charger` and the AGM power-off path (PMIC `RG_PWRHOLD`), so the kid can charge it and turn it off.

**Exit:** tap and swipe something on the screen, charge it, and power it off from the UI.

## Phase 4: the toy (1-3 days)

- [ ] postmarketOS via pmbootstrap (AGM used device `qemu-aarch64`, OpenRC, kernel and initramfs from our `boot.img`), root image at `/data/linux.img`.
- [ ] UI: no phone shell. A full-screen kiosk (cage or Sway) running a small launcher built for 480x640 with big buttons: clock, stopwatch, drawing pad, a couple of simple games, music player.
- [ ] Software rendering is fine at this resolution. GPU (Zink over the PowerVR Vulkan driver) is optional.

**Exit:** the kid can use it without help.

## Phase 5: bonuses (open-ended, any order)

| Feature | Source | Notes |
|---|---|---|
| CPU frequency and idle | AGM tree as is | Same SoC; efuse picks the OPP table |
| Audio | AGM AFE + `mt6357` codec | Speaker amp may differ from their `aw8738` |
| Wi-Fi | AGM `mt6739-consys` + staging gen2 driver | Firmware from our `/system/vendor/firmware`, NVRAM from `nvdata` (`APCFG/APRDEB/WIFI`) |
| Vibrator | stock DT `vibrator@0` | Small job, kids like it |
| Accelerometer | mainline `bma400`? | Check the exact chip first (`bma4xy_acc`) |
| GPU | AGM `powervr` + Mesa | Same GE8100 |

Out of scope: calls and SMS (the AGM modem work is LTE data only), Bluetooth, GPS, cameras,
heart rate, NFC, the Nordic co-processor.

## Phase 6 (optional): make Linux the default

Only once Phase 4 is solid. Options: put AGM's U-Boot in `boot` with a menu (Linux / Android),
or swap the images and start Android from `recovery`. Stock Android 7 in `recovery` mode probably
won't start the normal system, so U-Boot is the likely route.

## Fallback: Android toy

If any phase stalls: `dd` the backed-up `recovery.img` back, keep the debloat, and add a kid
launcher. No SIM, and Wi-Fi off or on a restricted network. Before handing it over, finish
checking for the Wiite IMEI/SIM upload that the DM100 research found (`watch.wiiteer.com`),
and disable whatever sends it.

## Decisions for you

1. Phase 1 unlock: probably not needed now. Only if LK rejects a repacked image.
2. Open the case for UART? Optional, but it saves days in Phase 2.
3. The kid's UI in Phase 4: I'd start with a simple custom launcher rather than Phosh or Sway.
