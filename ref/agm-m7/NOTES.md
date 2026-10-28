# AGM M7 notes

## Hardware

- SoC: MediaTek MT6739 (4x Cortex-A53, GICv3, PowerVR GE8100).
- RAM: 1 GiB at 0x40000000. eMMC: 7.28 GiB (FN62BB).
- PMIC: MT6357.
- Panel: GalaxyCore GC9306 (`gc9306_dbi_c_qvgal`), 240x320, DBI command mode.
- Stock firmware: Android 8.1, 32-bit kernel 4.4.95 (`bootopt=64S3,32S1,32S1`).

## Bootloader (LK)

LK loads at 0x46000000. Facts found by disassembly:

- `boot.img` is header v0. The kernel blob is `Image.gz` (or `zImage`) with the DTB appended.
- `bootopt=64S3,32N2,64N2` in the header cmdline selects a 64-bit kernel. The kernel address must be 512 KiB aligned.
- LK loads the DTB from the `boot` or `recovery` partition early, before fastboot. So `fastboot boot` runs our kernel with the DTB of the `boot` partition. Do not use it.
- LK applies the ODM overlay from `odmdtbo` to our DTB, and asserts (silent hang before the logo) if that fails. Our DTB is built with `-@` and has the node `lk-odm-overlay-sink` with all 41 labels that the overlay needs. Do not reuse those labels (for example `pio`) on real nodes.
- `fastboot getvar` hangs LK. `fastboot reboot recovery` boots normally.
- LK passes the framebuffer in `/chosen` as `atag,videolfb-*`.

## Test loop

Since 2026-10-14 U-Boot is in `boot` (`mmcblk0p25`) and LineageOS recovery is in `recovery` (`mmcblk0p2`). LK starts U-Boot in normal boot, so the modem is loaded for every OS that U-Boot starts. U-Boot shows a menu for 3 s (NixOS, NixOS generations, LineageOS, LineageOS recovery, Fastboot, Reboot) and then boots NixOS with extlinux from the FAT `vendor` partition (`/boot`). LineageOS `boot.img` and `recovery.img` are in `/boot/android/`. `reboot recovery` makes LK start LineageOS recovery directly, without U-Boot; that is the way to repair `boot` (root ADB after "Advanced > Enable ADB"). See `UBOOT-RESEARCH.md`. Old NixOS `boot.img`: `backup/boot-2026-10-14/`.

In recovery boot mode the vendor rule in `init.mt6739.usb.rc` never binds the USB gadget, so LineageOS has no USB ADB. `/system/etc/init/agm-m7.rc` on LineageOS has a fallback: after `sys.boot_completed=1` with `ro.bootmode=recovery`, it links `ffs.adb` and writes the `UDC`. Root (`phh-su`) works only from an ADB shell.

1. Boot LineageOS (GSI). Root is `phh-su -c`.
2. `scripts/build.sh` in the `agm-m7` OrbStack VM, then `KERNEL=vm scripts/mkboot-nix.sh` on the host.
3. `scripts/test-boot.sh <name>` writes `out/boot.img` to `boot`, runs `adb reboot`, waits for LineageOS, and copies pstore to `logs/<name>/`.

`fastboot flash boot out/boot.img` also works.

Faster, from a running NixOS: `scripts/nixos-flash.sh` copies `out/boot.img` over SSH, writes it to `boot`, and runs `systemctl reboot`. About 50 s per round trip.

- `reboot recovery`: the RTC driver sets `RTC_PDN1` bit 4 and triggers `WRTGR`, as the vendor kernel does. LK boots `recovery` (LineageOS) once and clears the bit. `reboot bootloader` sets bit 13 (fastboot).
- The USB gadget needs fixed MAC addresses (`g_cdc.dev_addr`, `g_cdc.host_addr`). With random ones, macOS often does not create the network interface after a reboot.

## Debug without UART

- `earlycon=pstoreram,0x4445f000` writes the kernel log into the stock pstore console zone. LineageOS shows it as `/sys/fs/pstore/console-ramoops`. RAM survives normal resets and long-press resets. A battery pull clears it.
- `mtk_wdt_early=0x10007000,30` arms the watchdog early. A hung kernel resets after 30 s.
- USB: `g_cdc` gives a serial shell (`/dev/cu.usbmodem*`) and ECM networking. The phone is 172.16.42.1. Set the Mac to 172.16.42.2/24. `scripts/tnet.sh` runs commands over telnet.
- The initramfs reboots after 300 s unless `/tmp/stay` exists.

## Restart

PSCI `SYSTEM_RESET` and `SYSTEM_OFF` both hang in the ATF. The watchdog is stopped during shutdown, so a hung power off needs a battery pull (a long press did not help). We power off through the PMIC instead: clear `RG_PWRHOLD` (`0xa08` bit 0), as the vendor kernel does. With USB connected, the phone then starts offline charging. The `mtk_wdt` restart handler must run first (`mediatek,lk-normal-restart`, priority 192). It sets `AUTO_START` (the bootloaders read it as "boot without power key") and clears `DDR_RESERVE` (else the bootloaders stall on the orange-state screen).

## userdata

`userdata` (`mmcblk0p33`) has full-disk encryption: dm-crypt `aes-cbc-essiv:sha256` with a 128-bit key, f2fs inside.

- `tools/dmtable.c` (static armhf, run as root on LineageOS) prints the dm table with the key. Keep it in `secrets/userdata-dm-table.txt`. Never commit it.
- `tools/dmctl.c` creates the mapping from that table without libdevmapper. NixOS used it to unlock `userdata` in the initrd until the root moved to the microSD card. NixOS does not touch `userdata` now.
- Android runs `fsck.f2fs` on the raw encrypted partition at every boot, and it always fails with "Magic Mismatch". That is normal.

## postmarketOS

- Built with pmbootstrap in the VM: device `qemu-aarch64`, kernel `lts`, `service_manager = openrc`, UI `console`. The root image goes to `/data/pmos.img` (2 GiB ext4).
- The initramfs mounts it with `losetup` and `mount -t ext4` (busybox `mount -o loop` fails), moves `/data` to `/mnt/data`, and runs `switch_root`.
- The initramfs boots `/data/nixos.img` first, then `/data/pmos.img`. `/data/<name>.disable`, or `touch /tmp/noboot` within 10 s, skips an image.
- `ssh user@172.16.42.1`.
- Do not upstream: pmOS (Nura) and Mobile NixOS do not accept AI-written code.

## NixOS

- `flake.nix` builds NixOS 26.05 for aarch64 on the Rosetta builder. The kernel and the initramfs still come from `boot.img` (`boot.kernel.enable = false`, `boot.initrd.enable = false`).
- Root is the whole microSD card: GPT, one ext4 partition with the partition label `nixos`. The initrd waits 15 s for it. Without the card, the initrd fails and reboots into LineageOS.
- Migration from `/data/nixos.img`: `sfdisk` and `mkfs.ext4` on the card, `nixos-deploy.sh boot` with the new initrd, then `cp -ax / <card>` with `nix-daemon` stopped (3.3 GB in 3.5 min), then flash `boot.img`. The old image is still on `userdata`.
- A full root makes the phone's auto-GC delete paths during `nix copy`, which shows up as "path ... is not valid".
- `scripts/nixos-deploy.sh [boot|switch]` copies a new system closure over SSH, sets the system profile and runs `switch-to-configuration`. `/sbin/init` on the root points to that profile, so `boot` takes effect at the next boot.
- Memory: with U-Boot in `boot`, LK runs in normal mode and reserves the modem memory, so NixOS has about 913 MiB. Large copies over the USB network then filled the page cache, and atomic allocations in the MUSB receive path failed and dropped the connection. `vm.min_free_kbytes = 16384` (`nixos/kernel.nix`) fixes it.
- Bootloader module (`nixos/bootloader.nix`, `boot.loader.agm-m7`): `installBootLoader` writes `extlinux.conf` and the last 8 generations to `/boot` (FAT on `vendor`) for U-Boot; "NixOS generations" in the U-Boot menu picks an older one. `system.build.bootImage` still builds an LK `boot.img`, and `boot.loader.agm-m7.device` would write it to a partition (null now, since U-Boot is in `boot`).
- `ssh root@172.16.42.1`. `ttyGS0` has a root autologin. systemd-networkd gives the Mac 172.16.42.2 over DHCP.
- The kernel needs `NETDEVICES` (else `SO_BINDTODEVICE` fails and networkd, resolved and sshd break) and nftables. `MEMCG` crashes the kernel when systemd loads BPF programs, so it stays off.

## PMIC

- The pwrap registers are the same as on MT6765, but the preloader also enables the INT1 interrupt group. The mainline MT6765 entry ignores INT1, and the pwrap interrupt then storms until the watchdog fires. The MT6739 entry turns INT1 off.
- eMMC: Samsung eMMC 5.1 (`FN62BB`), HS200/HS400 at 1.8 V. The I/O voltage is fixed at 1.8 V, so no `vqmmc` is needed. Stock runs HS400 at 200 MHz. We run HS200 (192 MHz from `msdcpll`, about 156 MB/s raw, 120 to 140 MB/s through dm-crypt and the loop image; plain high-speed mode gave 44 MB/s). HS400 with `mt6779` registers fails: the strobe delay scan in `PAD_DS_TUNE` (base + 0x188) finds no working value, and the card is then not initialised at all. Stock maps a second block, the MSDC0 pad macro at 0x11cd0000, which probably holds the strobe delay.
- microSD: MSDC1, `vmch` (3.3 V) as `vmmc`, `vmc` as `vqmmc` (switches to 1.8 V for UHS-I). There is no card detect line. The stock `cd-gpios` is GPIO15, but GPIO15 is keypad column 7, and the stock overlay sets `non-removable`. We do the same, so the kernel scans the slot once at boot. A 32 GB SDHC card runs SDR104 at 192 MHz: about 70 MB/s direct read, 77 MB/s buffered, 22 MB/s write. The MSDC1 delay lines are in a second block at 0x11c40000 (like MT8183, `top_base`). Without it the SDR104 tuning map is all ones, the delays do nothing, and the card gets data CRC errors (events 0xa040), CMD12 recovery and resets under load. The `cmd=12` timeouts in the log are only the recovery after such an error.
- The PMIC interrupt is EINT 182 (virtual pin 168), level high.
- The pwrap reset is infracfg `0x140` bit 0.
- Long-press reset: the mainline keys driver turns it off unless the DT sets it. We keep the stock setting: one key, `power-off-time-sec = <0>`. MT6357 has the MT6331 layout of `TOP_RST_MISC` (0x14c): time bits 13:12, power-key reset enable bit 9, home-key bit 8. Stock value 0x221.
- Power key: press and release have separate interrupts (`PWRKEY`, `PWRKEY_R`). `TOPSTATUS` (0x24) bit 1 is 1 when released.
- The mainline auxadc scale did not divide by the ADC resolution. Fixed. `bat_adc` gives the battery voltage (`adc-battery`, matches the stock kernel within about 60 mV while charging). `batt_v` reads garbage, and some channels time out.
- Battery percent: `ocv-capacity-table-0` comes from the stock `battery0_profile_t1` (25 °C) in `bat_gm30`. Each profile entry is (depth, OCV in 0.1 mV, resistance).
- Charger detection: `RGS_CHRDET` is `CHR_TOP_CON0` (0xa88) bit 4. Use the `CHRDET_EDGE` interrupt, as stock does. `CHRDET` is a level interrupt and fires all the time while a charger is connected. Do not use the label `charger`: the LK overlay sink has it.
- Charging: an external HL7005 (bq24157 family) at I2C1 0x6a charges the battery. LineageOS binds driver `hl7005` to `1-006a` and leaves the MT6357 charger off (`CHR_EN` = 0, `CHRWDT_OUT` = 1). Programming the MT6357 charger did nothing useful: the charge current we saw came from the HL7005, still set up by LineageOS. LineageOS has `/sys/devices/platform/mt-pmic/pmic_access` to read PMIC registers (write the address, then read).
- Gauge current: `FGADC_CUR_CON0` (0xd8a), latched with `FGADC_CON1` (`SW_READ_PRE`, wait for bit 15, read, `SW_CLEAR`). s16, 305.176 uA per LSB with 10 mOhm, times `CAR_TUNE` 0.93. Positive is charging.
- Battery NTC: 10 kOhm with 16.9 kOhm pull-up to 1.8 V (vendor default), on auxadc `batt_temp`. Our value matched LineageOS within 0.5 C.
- auxadc fixes for MT6357: `ISENSE` and `VCDT` used request bit 0 (should be 1 and 2), and `ISENSE` is 15 bits, not 12.
- The stock kernel also reads a wrong date from the RTC. Android keeps the time in software.

## Keypad

The keypad is an 8x9 matrix (key = row * 9 + col). The hang-up key is the PMIC power key. The PMIC home key is not connected.

| Row, col | Key |
|---|---|
| 0,1 | side key (`KEY_F11`) |
| 3,3 / 5,3 | left / right soft key (`KEY_MENU` / `KEY_BACK`) |
| 4,3 / 5,4 / 4,4 / 3,4 / 6,3 | D-pad up / right / down / left / center (`KEY_ENTER`) |
| 7,3 | call (`KEY_SEND`) |
| 7,4 3,5 6,4 4,5 5,5 6,5 7,5 3,6 4,6 | 1 to 9 |
| 5,6 / 6,6 / 7,6 | * / 0 / # |

## Display

LK turns on the panel and the path OVL0 -> RDMA0 -> DBI0 (mutex 0). The panel has its own frame memory, so each frame needs a trigger. The `mt6739-dbi` DRM driver keeps LK's setup and, for each update:

1. waits until DBI `STA` (0x14013000) bit 4 (busy) is 0,
2. sets OVL layer 3: `L3_CON` (0x090) format XRGB8888 with alpha off, `L3_PITCH` (0x0a4), `L3_ADDR` (0xfa0), and `SRC_CON` (0x02c) to layer 3 only,
3. writes `0x2c` to `SCMD0` (0xf80), pulses `START` (0x00c) bit 15, and sets mutex 0 `EN` (0x14001020).

- LK uses layer 0 for the logo and layer 3 (with per-pixel alpha) for the text. XRGB buffers have alpha 0, so alpha must be off.
- The mode comes from DBI `ROI_SIZE` (0x06c), which LK sets to 240x320.
- LK's framebuffer is at 0x77ee0000 (`atag,videolfb`, `mblock-4-framebuffer`, no-map). `/dev/mem` reaches it only through mmap (`devmem`), not `dd`.
- The DBI register layout is the same as the vendor `mt6771/dispsys/ddp_reg_dbi.h`.
- Backlight: `disp_pwm` (0x1100e000) on GPIO85 (function 1), the MT8183 register layout. The ODM DTBO sets `lcd-backlight` to `led_mode 5` (BLS PWM). LK leaves GPIO85 as a plain GPIO, driven high, with the PWM off. `disp_pwm_sel` runs at 104 MHz from `univpll2_d4` (LineageOS uses 26 MHz).
- The backlight node has a phandle, so `pwm-backlight` stays off until the display driver enables it. `mt6739-dbi` does that in `enable`, and in `disable` it sends DCS display off and sleep in (single commands go to `SCMD0`). `enable` sends sleep out, waits 120 ms and sends display on.

## CPU idle

- PSCI `CPU_OFF`/`CPU_ON` and `CPU_SUSPEND` power-down work in the stock ATF without the vendor MCDI driver. We use two stock states: core off (`0x10001`) and cluster off (`0x1010001`). The deeper stock states (SODI, deep idle, `0x1010002`..`0x1010005`) need the SPM firmware and are not used.
- Both states stop the local timer. The SYST timer (0x10017000, SPI 186, 13 MHz) is the broadcast device (`mtk-clkevt`).
- With `NO_HZ_IDLE` and the `menu` governor, the idle cores spend about 95% of the time in cluster off.

## CPU frequency

- LK leaves the CPU at about 950 MHz (ARMPLL_LL PCW 0x1244EC, post-divider 2) with Vproc 1.15 V and Vsram 1.25 V.
- The vendor driver picks the OPP table from the `atag,devinfo` efuse words in `/chosen`: segment `data[30]` and turbo `data[29]` bits 21:20. This phone has segment 0xad and turbo 3, so CPU level 0, the "FY" table: 299 MHz at 0.95 V to 1274 MHz at 1.306 V.
- `mediatek-cpufreq` drives it. ARMPLL_LL: CON0 0x1000c200, PCW in CON1 bits 21:0 (14 fraction bits), post-divider in bits 26:24. The CPU mux is in TOPCKGEN_AO (0x1001b000) bits 11:8: 0 = clk26m, 1 = ARMPLL. clk26m is the intermediate clock during PLL changes.
- Vsram = Vproc + 100 mV, clamped to 1.15..1.3125 V (vendor rule). Mainline `mt6357-regulator` had the wrong VOSEL mask (0x7f00 instead of 0x7f) for both VSRAM LDOs, so Vsram did not change. Fixed in our tree.

## I2C and touch

- MT6739 I2C uses the v1 register layout plus `LTIMING` at 0x2c. The vendor driver also sets `CLOCK_DIV` (low byte only), the `IO_CONFIG` "AED" field (0x1f << 4) and leaves `DCM_EN` at 0xe. We copy that.
- `i2cdetect` without `-r` probes with zero-length quick writes, which `i2c-mt65xx` does not support. The bus then looks empty. Use `i2cdetect -r` or `i2cget`.
- A GPIO trace (`/dev/mem` mmap of `GPIO_DIN` 0x10005240 in a tight loop) is enough to decode 100 kHz I2C on the wire.
- Touch: the ODM overlay says 0x2e, but the Telink TLSC6x answers at 0x38 with the FocalTech FT6x36 register layout (chip ID 0x64 at 0xa3). INT is GPIO 11 (EINT, falling), reset GPIO 51, supply `vldo28`.
- I2C1 has the HL7005 charger (0x6a). Its register 3 reads 0xf0 (vendor code 7), so the vendor driver's ID check (vendor 2) and mainline `bq2415x` do not match it. Register 5 bit 5 (`IO_LEVEL`) always reads 1. The safety register (6) only takes the first write after a chip reset; we set 1250 mA and 4.20 V. The overlay also lists sensors at 0x15, 0x30, 0x48, 0x68 and 0x77 there, but none answer.

## Graphical session

- labwc (replaced by Sway): a systemd service on tty1 (`PAMName=login`, user `user`), `WLR_RENDERER=pixman`. Autostart: `wvkbd-mobintl -H 130` and `foot` (DejaVu Sans Mono, size 6). `wvkbd`'s `-L` is the landscape height; portrait uses `-H`.
- Weston (not used): Weston 15 kiosk shell with `foot`. Weston's desktop shell crashed (SIGSEGV in `weston_desktop_xdg_surface_destroy`) after a protocol error from `weston-terminal`. Weston's on-screen keyboard needs `text-input-v1` clients (only its demo apps) and does not fit 240 px.
- Our DRM device has no vblank support, so the atomic helpers fake the flip-done events. Compositors work with that.
- Sway 1.12 / wlroots 0.20: the first `output * power on` after `power off` fails ("Backend commit failed") with no kernel commit and no wlroots error. The second one works, so swayidle sends it twice.
- `agm-home` is a background layer surface. It reads `/dev/input/event*` for the Menu then `*` lock combo, so it does not take keys away from Sway. `agm-lock` uses `ext-session-lock-v1`, so no input reaches the session while it runs.
- Look: Inter (static instances made from the variable font at build time, so each process loads about 1 MB instead of the 13 MB collection), a neutral dark palette for the shell, light apps, blue accent. Icons come from the icon theme by name: Adwaita symbolic icons are recolored, app icons keep their colors (SVG through resvg, else PNG).
- Sway always draws tab titles in tabbed layouts, even with `default_border none`. `agm-windows` moves each new window to a free workspace instead, so a window is alone and has no title bar.
- `agm-menu` reads `.desktop` files from `XDG_DATA_DIRS`. Sway binds the soft keys itself, so the menu switches Sway to the `menu` mode while it is open. Sway drops a mode without bindings, so `menu` has one (`XF86Send nop`).
- Sway 1.12 crashed in `constrain_popup` when t9 committed its input method popup after its text field lost focus to the menu: the function has no NULL check for the focused text input. `nixos/patches/sway-im-popup-no-text-input.patch` adds it.
- `agm-settings` is a normal xdg-shell window. It asks iwd for passphrases through its own agent object (`net.connman.iwd.Agent`): the app sets the passphrase before `Network.Connect`, so the agent answers at once. The password field uses `text-input-v3`, so t9 types into it.
- Shared user settings are in `~/.config/agm/settings` (`clock_24h`, `dim_after`, `off_after`). The idle script runs swayidle in a loop with these delays; the settings app kills swayidle to apply new ones.
- The time zone belongs to timedated now (`time.timeZone = null`, tmpfiles links `Europe/Warsaw` on a fresh root). A polkit rule lets `user` set it.
- The display driver allocates 300 KB contiguous buffers. Without CMA (and without compaction) these allocations failed after some uptime, and Sway could not turn the screen back on ("Failed to create DRM dumb buffer: Cannot allocate memory"). The kernel now has a 32 MiB CMA pool.
- iwd allows D-Bus access only for root and `wheel`. A small policy gives `user` access to iwd without `wheel`.

## T9 input method

- `tools/t9`: an `input-method-unstable-v2` client. It grabs the keyboard while a text field has focus, and sends the keys that it does not use back through `virtual-keyboard-unstable-v1`. `foot` and Firefox support `text-input-v3`.
- Sway runs its key bindings before the input method grab. `t9` switches Sway to a `text` mode while a text field has focus, and back to `default` after. Back is no longer bound in either mode (the OSK moved to F11), so `text` is now the same as `default`.
- The grab first sends a `no_keymap` keymap (format 0, size 0). wlroots answers a virtual keyboard keymap with size 0 with a `no_memory` error, which kills the client.
- Sway forwards only the keys of the input method's own virtual keyboard past the grab. Keys from `wvkbd` also go through `t9`, so its digit keys type T9 letters.
- Word lists: FrequencyWords (OpenSubtitles 2018, 50k words, CC-BY-SA 4.0). The English list splits contractions ("don" + "'t"), so `en-extra.txt` adds them.
- `checks.aarch64-linux.t9` runs a headless Sway (pixman) in the build sandbox, types into `foot` through a virtual keyboard, and takes screenshots with `grim`.

## systemd initrd

- LK puts `root=/dev/ram` on the kernel command line. Our part of the command line comes later and has `root=fstab`, which wins. NixOS's `boot.kernelParams` only reach the kernel through `boot.img`, so `boot-parts` writes them into `cmdline`.
- `init=/nix/var/nix/profiles/system/init`: the initrd resolves the symlink inside the new root, so `nixos-deploy.sh boot` is enough for userspace changes.
- Store paths in initrd unit scripts are not copied into the initrd. Use `boot.initrd.systemd.extraBin` (`dmctl` failed with status 127).
- A device-mapper device created without libdevmapper's udev handshake gets `SYSTEMD_READY=0` (on `add` events always). systemd then stops everything that depends on it. The old `userdata` setup needed a udev rule that set it ready.
- `emergency.service` writes the failed units and the journal to the kernel log (pstore), then runs `systemctl reboot -ff --reboot-argument=recovery`, so a failed boot ends in LineageOS and not in a boot loop.
- The initrd is 81 MB unpacked. With xz and without TPM2, FIDO2 and LVM it is 17 MB, and `boot.img` 22.4 MB. `boot` and `recovery` hold 24 MiB each.
- NixOS sets `loglevel=4`, which hides most messages from the pstore console. We use `boot.consoleLogLevel = 8`.
- Debug: `systemd.log_target=kmsg systemd.journald.forward_to_kmsg=1` on the command line puts the whole initrd log into pstore. `systemd.debug_shell=ttyGS0` did not give a shell on the USB serial port.

## Audio

- The MT6739 AFE (0x11220000, SPI 153) is a subset of the MT8183 AFE: all 280 registers that both have use the same offsets and bits. Our driver keeps DL1, DL2, VUL (`UL1`) and AWB (`UL2`) and the ADDA link. MT6739 has no audio power domain and only APLL1; ADDA runs from clk26m and needs no APLL.
- The AUDIOSYS clock gates (AUDIO_TOP_CON0/1) are in the AFE registers and share its regmap. The AFE must leave cache-only mode before it turns its clocks on, else the gate writes are dropped.
- Codec: the MT6357 (mainline `mt6357`, `vaud28`). Link: MTKAIF protocol 1 over GPIO 62-65 (MOSI) and 66-69 (MISO), mode 1.
- Speaker: MT6357 line out into an external amplifier. GPIO 24 sets it on with 3 pulses (`awinic,aw8738`, mode 3, like the vendor `Ext_Speaker_Amp_Change`). It is loud: line out step 8 of 18 was too loud for a test.
- Earpiece: the MT6357 handset output. Step 10 of 18 (0 dB) is a normal level.
- Main mic: AIN0 with MICBIAS0, `Mic Type Mux` = ACC. +24 dB gives about 11 % of full scale for speech. AIN1 and AIN2 give no signal. The phone has no headphone jack.
- Playback route: `ADDA_DL_CH1 DL1_CH1`, `ADDA_DL_CH2 DL1_CH2`, then `Line Out Source` (speaker) or `Handset Source` (earpiece) = `Playback`. Capture: `PGA L/R Mux` = AIN0, `UL1_CH1 ADDA_UL_CH1`, `UL1_CH2 ADDA_UL_CH2`, PCM `hw:0,2`.
- NixOS: PipeWire with WirePlumber and our UCM profile (`nixos/ucm2`, card driver `AGM_M7`): one sink with the Speaker and Earpiece routes, one Main Microphone source. `ALSA_CONFIG_UCM2` points to a merged copy of `alsa-ucm-conf`, because overriding that package rebuilds `alsa-lib` and everything that uses it. At the default 40 % volume the line out is at -10 dB and the rest is software volume. PipeWire and WirePlumber use about 18 MB RSS. WirePlumber needs a few seconds after login before the card shows up.
- Speaker and Earpiece share one PCM, so PipeWire makes them separate profiles ("HiFi (Mic, Speaker)" and "HiFi (Earpiece, Mic)"), not ports of one sink. Settings > Sound switches the output with `wpctl set-profile` and then polls until the new sink is there (1-2 s). Each output keeps its own volume; a new one starts at 40 %.

## Modem

- Firmware: `md1img` holds one `md1rom` (`MOLY_LR12A_R2_MP_V96_6`, not encrypted). `md1dsp` is the DSP image.
- LK loads both, sets the remap and the EMI MPU, and passes `ccci,modem_info_v2` in `/chosen`. It does this only in normal boot. In recovery mode it logs "recovery mode, no need load modem" and turns the modem off.
- Reserved memory from LK in normal boot: `mblock-8-ccci` 0x62000000-0x71ffffff (256 MiB, mapped), `mblock-7-ccci` 0x72000000-0x720fffff (shared memory, nomap), `mblock-9-ccci` 0x76000000-0x76bfffff (nomap). The stock kernel frees about 200 MiB of `mblock-8` that the modem does not use. We do not yet, so NixOS has 714 MiB instead of about 970 MiB.
- `/dev/mem` is blocked (strict devmem), so the modem memory can only be read from a kernel driver.
- Stock start order (LineageOS dmesg): `ccci_mdinit` starts the modem through an ioctl, the modem sends control message `0x5555FFFF` over CCIF, the AP sends the runtime data, then `ccci_fsd` serves NVRAM files from `nvdata`.

## Networking

- NetworkManager with `wifi.backend = "iwd"`. iwd keeps its own profiles in `/var/lib/iwd`; NetworkManager imported them. `usb0` stays with systemd-networkd and is unmanaged in NetworkManager.
- The modem starts at boot from `ccci-fsd.service`. It works on a copy of the NVRAM in `/var/lib/ccci-fs/data`; the phone's own partitions are mounted read-only.

## Hardware status

| Part | Status |
|---|---|
| CPUs, GIC, timer, PSCI | works |
| Clocks (apmixedsys, topckgen, infracfg) | works, `clk_ignore_unused` on cmdline |
| Pinctrl, GPIO, EINT | works |
| eMMC | works, 52 MHz, `vemc` from the PMIC |
| USB device | works, PIO only |
| Watchdog, restart | works |
| Display | works (`mt6739-dbi` DRM, fbcon), relies on LK's setup |
| Keypad, power key | works (`mt6779-keypad`, `mtk-pmic-keys`) |
| I2C0, I2C1 | works (`i2c-mt65xx`, own MT6739 entry) |
| Touchscreen | works (`edt-ft5x06`) |
| PMIC (pwrap, MT6357 regulators, RTC, auxadc) | works, `regulator_ignore_unused` on cmdline |
| Power off | works (PMIC `RG_PWRHOLD`) |
| Battery voltage, percent | works (`adc-battery` with the stock OCV table) |
| Charger detection, fuel gauge | works (`mt6357-charger`) |
| Charging | works (`hl7005`, external charger on I2C1) |
| Userspace | NixOS 26.05, root on the microSD card |
| Audio (speaker, earpiece, main mic) | works (own MT6739 AFE, `mt6357`, `aw8738`) |
| microSD | works, SDR104, scanned only at boot, NixOS root |
| Wi-Fi, BT, modem, camera, GPU | not done |
