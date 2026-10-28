# U-Boot research

Question: can mainline U-Boot run as the "kernel" of our `boot.img`, started by the stock LK, and give us fastboot over USB?

Short answer: yes, this is very likely possible. U-Boot already has almost all the parts. The main cost is a new SoC target, a small clock driver, and blind bring-up without UART.

Source: mainline U-Boot, shallow clone in `ref/u-boot` (commit `fed92546`, 2026-10-12).

Legend: **[src]** = verified in U-Boot or Linux source. **[inf]** = inference, not tested.

## 1. USB: MUSB gadget and fastboot

- **[src]** `drivers/usb/musb-new/mt85xx.c` (`CONFIG_USB_MUSB_MT85XX`) is a MediaTek MUSB glue with gadget support. Compatible: only `mediatek,mt8518-musb`. Users: `mt8516_pumpkin_defconfig` and `mt8518_ap1_emmc_defconfig`. `mt8516.dtsi` uses `"mediatek,mt8516-musb", "mediatek,mt8518-musb"`.
- **[src]** Both defconfigs enable `CONFIG_USB_FUNCTION_FASTBOOT`, `FASTBOOT_FLASH`, `FASTBOOT_FLASH_MMC_DEV=0` with `USB_MUSB_GADGET` + `USB_MUSB_MT85XX`. So fastboot over musb-new on MediaTek is an upstream configuration. Sunxi also enables fastboot on MUSB by default (`drivers/fastboot/Kconfig`).
- **[src]** The glue uses the legacy gadget path (no `DM_USB_GADGET`). The board probes the device in `board_late_init()` with `uclass_get_device(UCLASS_USB_GADGET_GENERIC, 0, ...)` (`board/mediatek/mt8516/mt8516_pumpkin.c`). Interrupts are polled through `dm_usb_gadget_handle_interrupts()` in `musb_uboot.c`, which calls the glue ISR. The ISR clears the MTK write-1-to-clear interrupt registers. PIO only, no DMA.
- **[src]** The glue programs the PHY with hard-coded byte writes at `USB20_PHY_BASE 0x11110800` (MT8516). The offsets (0x18 ACR6, 0x68 DTM0, 0x6c/0x6d DTM1) are the T-PHY v1 U2 COM bank layout. The writes to 0x6c/0x6d force IDDIG, VBUSVALID and AVALID, so the PHY is forced to device mode.
- **[src]** Our PHY is the same layout: Linux `u2port0` is at `0x11cc0800`, and T-PHY v1 puts the U2 COM bank at `port_base + 0x000`.
- **[src]** The glue needs three clocks named `usbpll`, `usbmcu`, `usb`. Our Linux DT uses `main`, `mcu`, `univpll`.
- **[src]** U-Boot has `drivers/phy/phy-mtk-tphy.c` with `mediatek,generic-tphy-v1`. It has `init`, `power_on`, `power_off`, but no `set_mode` (Linux has `u2_phy_instance_set_mode()` for IDDIG). The MUSB glue does not use the generic PHY API at all.

What we must add for MT6739:

1. Glue: add `mediatek,mt6739-musb` (or `mediatek,mtk-musb`). Take the PHY base from DT, not from a constant. Quick hack **[inf]**: change `USB20_PHY_BASE` to `0x11cc0800`; the existing byte sequence will very likely work. Clean way: use `generic_phy_get_by_index()` + T-PHY, and add `set_mode` (port 15 lines from Linux) to the U-Boot T-PHY driver.
2. Clocks: either a U-Boot clock driver for the needed gates and muxes, or `fixed-clock` stubs plus a few raw writes in board code (infra gate `CLK_INFRA_ICUSB` = INFRA CG0 bit 8, clear register `0x10001084`; `usb_top_sel` parent `univpll3_d4`). **[inf]** LK does not use USB in normal boot, so we cannot trust that it left the USB clock on.
3. The rest (musb core, gadget, f_fastboot, g_dnl) is generic.

## 2. MediaTek arm64 SoCs in U-Boot

- **[src]** `arch/arm/mach-mediatek`: MT7622, MT7981, MT7986, MT7987, MT7988, MT8183, MT8188, MT8189, MT8195, MT8365, MT8512, MT8516, MT8518 (arm64), MT7623, MT7629 (arm32). No phone SoC: no MT6735, MT6739, MT6761, MT6765 anywhere in `drivers/` or `arch/`.
- Closest by block:
  - USB: **MT8516/MT8518** (same MUSB + same U2 PHY layout). **[src]**
  - MSDC: **MT8183** compat data. Linux `mt6779_compat` equals `mt8183_compat` except `needs_top_base`. In U-Boot `top_base` is optional (`if (host->top_base)`). **[src]** U-Boot has no `mediatek,mt6779-mmc`, so we add one entry (copy `mt8183_compat`, maybe without `use_dma_mode` at first).
  - PMIC wrapper: **MT8365** (`drivers/power/pmic/mtk-pwrap.c` already knows the **MT6357** PMIC). We would add an MT6739 pwrap register table from our Linux `mtk-pmic-wrap.c`. Optional.
  - Clocks/pinctrl framework: `drivers/clk/mediatek/clk-mtk.c` and `drivers/pinctrl/mediatek/pinctrl-mtk-common.c` are generic. Our Linux `clk-mt6739.c` and `pinctrl-mt6739.c` can be ported to them.
- Minimum for a new SoC:
  - `TARGET_MT6739` in `arch/arm/mach-mediatek/Kconfig`, `mt6739/init.c` (`dram_init`, usable RAM top, `reset_cpu`), `board/mediatek/mt6739/`, `include/configs/mt6739.h`, a defconfig and a DTS. `MTK_MEM_MAP_DDR_SIZE` must cover 1 GiB (default is 512 MiB).
  - Timer: none. The arm64 generic timer works from `CNTFRQ_EL0`. **[src]** U-Boot writes `CNTFRQ` only at EL3, so it depends on ATF having set it (see risks).
  - GIC: none. U-Boot polls; it does not use interrupts.
  - Pinctrl: none at first **[inf]**. LK has already muxed the eMMC pins. Needed later for the SD card if LK does not set it up.
  - MSDC: one compat entry, a `source` clock with a correct rate (`clk_get_rate()` is used for the divider), and `hclk`.
  - Watchdog: `drivers/watchdog/mtk_wdt.c` (`mediatek,wdt`). **[src]** Its reset does not set `AUTO_START` or clear `DDR_RESERVE`; we must do that, as our Linux restart handler does. Do not copy `reset_cpu()` from MT8516: it uses PSCI `SYSTEM_RESET`, which hangs in our ATF.
  - Serial: not needed (no UART access).

## 3. Prior art

- I found no public mainline U-Boot that is chainloaded from MediaTek LK on a phone. Searches for MT6735/MT6765/MT6739 + U-Boot only found LK reverse engineering (Rabbit R1 boot notes, `lk-mt6765` forks) and the mt6735-mainline group (Linux only).
- **[src]** Very close prior art in U-Boot itself, same idea on other vendors:
  - `ARCH_SNAPDRAGON` (`doc/board/qualcomm/board.rst`): U-Boot as the kernel of an Android `boot.img`. Recipe: `gzip u-boot-nodtb.bin`, append the DTB, `mkbootimg`. Uses `OF_BOARD`, `SAVE_PREV_BL_FDT_ADDR`, `LINUX_KERNEL_IMAGE_HEADER`, fastboot.
  - `exynos-mobile` (`doc/board/samsung/exynos-mobile.rst`): Samsung phones, started by S-BOOT **or S-LK**, `u-boot-nodtb.bin` in `boot.img`, fastboot over USB, `POSITION_INDEPENDENT`.
  - `ARCH_MMP` (Marvell PXA1908 phones): selects `SAVE_PREV_BL_FDT_ADDR` and `SAVE_PREV_BL_INITRAMFS_START_ADDR`.

## 4. Build U-Boot as a kernel-like payload

- **[src]** `CONFIG_LINUX_KERNEL_IMAGE_HEADER` puts an arm64 `Image` header (`b reset`, text_offset, image_size, `ARM\x64` magic) at `_start` (`arch/arm/cpu/armv8/start.S`, `asm/boot0-linux-kernel-header.h`). It has no prompt; the target selects it. `LNX_KRNL_IMG_TEXT_OFFSET_BASE` sets text_offset (`TEXT_BASE` minus this base).
- **[src]** `CONFIG_POSITION_INDEPENDENT=y` (MT8516 Pumpkin, qcom, exynos-mobile use it). U-Boot can then run from wherever LK puts it (`0x40200000`) and relocate itself.
- DTB, **[src]**:
  - `SAVE_PREV_BL_FDT_ADDR` adds `save_boot_params()`, which keeps `x0` from the previous loader. `get_prev_bl_fdt_addr()` returns it, and the env gets `prevbl_fdt_addr`.
  - `OF_BOARD` + `board_fdt_blob_setup()` lets the board choose the control DT: the internal one, or the one in `x0`. Snapdragon and exynos-mobile both prefer the internal DT and fall back to `x0`.
- Proposed packing **[inf]**:
  - `boot.img` kernel = `gzip(u-boot.bin)` + `mt6739-agm-m7.dtb`.
  - `u-boot.bin` has U-Boot's own DT at its end (`OF_SEPARATE`).
  - The appended Linux DTB is only for LK. It must keep `-@` and the `lk-odm-overlay-sink` node, else LK asserts.
  - LK applies its overlay and fills `/memory`, `/chosen` (`atag,videolfb`, modem info), and reserved memory. Then it passes that DTB in `x0`. U-Boot reads RAM and framebuffer data from it.
  - Our `scripts/mkboot.py` needs no change. Only the "kernel" file changes.
- Booting Linux afterwards **[inf]**: U-Boot loads the Linux `Image`, DTB and initrd from another partition (or a FIT, or a v0 `boot.img` via `CONFIG_ANDROID_BOOT_IMAGE`). LK puts runtime data in its DTB, so U-Boot must copy it into the Linux DTB in `ft_board_setup()`: `/memory`, reserved memory (`mblock-*`, CCCI, CONSYS), `/chosen` `atag,videolfb*`, `ccci,modem_info_v2`, and the LK part of `bootargs`. Simpler first step: boot Linux with the `x0` DTB as is.
- Bonus: U-Boot `fastboot boot` takes a `boot.img` from RAM. That fixes our current problem where LK's `fastboot boot` uses the wrong DTB.

## 5. Effort and risks

Rough effort (evenings, with blind debugging), **[inf]**:

| Step | Effort |
|---|---|
| Target skeleton, defconfig, DTS, `dram_init`/RAM top from `x0` DTB, WDT reset | 1 to 2 days |
| Proof of life without UART (screen or RAM log, see below) | 0.5 to 1 day |
| DBI video driver for an on-screen console (port of the 300-line Linux `mt6739-dbi`, `video_sync` triggers `DBI_START`) | 1 day |
| Clocks: stubs + raw writes, or a subset port of `clk-mt6739.c` | 1 to 3 days |
| MSDC0 compat entry and clock rate | 0.5 day |
| MUSB glue for MT6739, PHY, fastboot to eMMC | 1 to 3 days |
| Boot Linux with DT fixups from the LK DTB | 1 to 2 days |
| Optional: pwrap + RTC reboot modes, pinctrl, SD card | 2 to 4 days |

Total: about 1 to 2 weeks of part-time work for "LK -> U-Boot -> fastboot flash -> boot Linux". Most of the risk is in the first step, because we cannot see anything.

Main risks:

1. **No UART.** Bring-up is blind. Options:
   - Draw to the LK framebuffer (`0x77ee0000`) and trigger `DBI_START`. The panel has its own frame memory, so nothing shows without the trigger.
   - Write a log to a fixed RAM area (for example a custom `DEBUG_UART` putc into the pstore region). Then reset with the watchdog, which keeps DRAM, boot Linux and read the log with `/dev/mem`.
   - USB CDC ACM console (`CONFIG_USB_FUNCTION_ACM` exists) once USB works.
2. **LK handoff details unknown.**
   - Exception level (EL1 or EL2).
   - Whether `CNTFRQ_EL0` is set. Our Linux DT has `clock-frequency = <13000000>` in the timer node, which hints that firmware may not set it. If it reads 0, U-Boot delays break and we must override the timer rate.
   - Whether LK checks anything in the Image header. **[inf]** It boots our `Image.gz` + DTB, so U-Boot with the same header should pass.
   - Check EL and CNTFRQ from Linux first (dmesg "CPU: All CPU(s) started at ELx", and a small `mrs cntfrq_el0` test).
3. **Memory map.** U-Boot relocates to the top of RAM. The top of RAM has the LK framebuffer (`0x77ee0000`), CCCI regions (`0x62000000`..`0x76bfffff` in normal boot), CONSYS memory and maybe ATF/TEE. We need `board_get_usable_ram_top()` below `0x62000000`. The fastboot buffer must also avoid these regions.
4. **Reset and power off.** PSCI reset and off hang in our ATF. U-Boot must use the watchdog with `AUTO_START` set and `DDR_RESERVE` cleared, else the bootloaders stall. Reboot to recovery or fastboot needs RTC `PDN1` bits through the pwrap, which is not in U-Boot for MT6739 yet.
5. **Clocks LK leaves off.** USB (and SD) may need gates and muxes that LK did not turn on in normal boot. MSDC needs the right source clock rate for the divider.
6. **Speed.** The MUSB glue is PIO only **[inf]**: a few MB/s. Good for `boot.img`, slow for a full root image.
7. **Brick risk is low but real.**
   - Test U-Boot from `recovery` first: LK boots it with `adb reboot recovery` or RTC bit 4, and `boot` keeps NixOS.
   - Stock LK fastboot stays available through RTC bit 13 / `reboot bootloader`.
   - mtkclient (akos's laptop) is the last fallback.
   - Do not let U-Boot overwrite the partition it runs from until fastboot is proven.

## 6. Boot menu design (lk2nd-like)

Goal: U-Boot in `boot`, started by LK in normal boot. It shows a menu with NixOS, LineageOS, LineageOS recovery and fastboot, and has its own fastboot.

### 6.1 What we get from LK's normal boot

- **[src]** LK loads and starts the modem only in normal boot (see `NOTES.md`). U-Boot runs in normal boot, so every OS that U-Boot starts gets the modem. Today LineageOS boots in recovery mode and has no modem. U-Boot fixes that.
- Both OSes use our 64-bit mainline kernel with the same `mt6739-agm-m7.dtb`. The LineageOS DTB only adds `/firmware/android` (`mt6739-agm-m7-android.dts`). So U-Boot can pass the `x0` DTB (with LK's runtime data) to both kernels. For LineageOS it adds `/firmware/android` with `fdt` commands. **[inf]** No 32-bit kernel and no vendor DTB are involved; this removes the hardest problem.
- LK charger mode also boots `boot`, with `androidboot.mode=charger` in the `x0` `/chosen/bootargs`. U-Boot must see this and boot the default OS at once, without the menu.

### 6.2 Input

- **[src]** The phone has a D-pad on the keypad matrix: `KEY_UP` (4,3), `KEY_DOWN` (4,4), `KEY_ENTER` (6,3), plus `KEY_LEFT`, `KEY_RIGHT`, `KEY_BACK` and the digits (`mt6739-agm-m7.dts`). So `bootmenu` can use real arrow keys. No volume-key hacks are needed.
- The side button is keypad position (0, 1), `KEY_F11` in our keymap. It is a good "hold at power on" key for U-Boot: hold it to stop autoboot and show the menu.
- **[src]** U-Boot has no MediaTek keypad driver. The keypad state is 5 read-only registers at `0x10010004` (`KPD_MEM1..5`, bits 15:0 of each, a bit is 0 when the key is down). The clock is `clk26m`.
- **[src]** `drivers/input/button_kbd.c` turns `UCLASS_BUTTON` devices with `linux,code` into a keyboard (stdin), and maps arrows to the escape codes that `bootmenu` reads.
- Plan **[inf]**: a small `button-mtk-keypad` driver (one child node per key, with row and column), or a `UCLASS_KEYBOARD` driver that polls `KPD_MEM`. About 100 lines either way. LK uses the keypad for its boot mode keys, so the keypad is very likely powered and configured.

### 6.3 Display

- The menu needs the DBI video driver from section 5 (port of `mt6739-dbi`). The panel is 240 px wide: with the 8x16 font that is 30 columns, enough for `bootmenu`.
- Before the video driver works, the menu cannot be seen. A safe default (autoboot NixOS after a few seconds) keeps the phone usable during bring-up.

### 6.4 Where each image lives

Decided (2026-10-14): the eMMC `vendor` partition (504 MiB) becomes a FAT boot partition. LineageOS 24.0 has its own `vendor` in `super` on the microSD, and NixOS takes its 5 firmware files from the `agm-m7-firmware` repo, so nothing uses `vendor` now. The GPT label stays, so LK sees no change. Backup: `backup/vendor-2026-10-14/vendor.img`.

| Entry | Image | Place | How U-Boot starts it |
|---|---|---|---|
| U-Boot | `boot.img` (U-Boot + DTB for LK) | eMMC `boot` (24 MiB) | LK, normal boot |
| NixOS | kernel, initrd, DTB per generation | `vendor` FAT: `/extlinux/extlinux.conf` and `/nixos/` | `bootmeth_extlinux`, from `boot.loader.generic-extlinux-compatible` |
| LineageOS | `boot.img` (header v0) | `vendor` FAT: `/android/boot.img` | `fatload` + `bootm` (`CONFIG_ANDROID_BOOT_IMAGE`) |
| LineageOS recovery | `recovery.img` | eMMC `recovery` | `bootm` from U-Boot, or set `RTC_PDN1` bit 4 and reset, so LK boots it |
| Fastboot | none | none | `fastboot usb 0` |
| U-Boot env | `uboot.env` | `vendor` FAT | `ENV_IS_IN_FAT` |

- Everything U-Boot reads is on the eMMC. U-Boot needs only MSDC0, not the microSD and not the PMIC regulators.
- FAT, not ext4: U-Boot must write the environment (to clear `boot_once`), and its FAT write is safer than its ext4 write.
- **[src]** With extlinux, the menu can also list the NixOS generations. That fixes "LK has no menu, so older generations cannot be picked at boot" (`NOTES.md`). `nixos/bootloader.nix` would then stop writing `boot`. About 27 MB per generation, so a limit of 8 generations uses about 220 MB.
- `recovery` keeps a real recovery image, so LK's own recovery path ("reboot recovery", RTC bit 4) still works without U-Boot.

### 6.5 Fastboot

- **[src]** U-Boot fastboot finds partitions by GPT name on `FASTBOOT_FLASH_MMC_DEV` (eMMC, `mmc 0`). Other devices use `dev:part`, for example `fastboot flash 1:super super.img` for the microSD (`do_get_part_info()` in `drivers/fastboot/fb_mmc.c`). Sparse images are supported (`IMAGE_SPARSE`).
- **[src]** `fastboot boot` works on a full `boot.img` in RAM, with the right DTB.
- Aliases (`fastboot_partition_alias_lineage=...`) give short names.
- **[inf]** PIO only: a 500 MiB sparse `super` takes minutes. For `super`, `scripts/android-flash.sh` over SSH from NixOS stays faster.
- Safety: LK fastboot stays (RTC `PDN1` bit 13, `reboot bootloader` from Linux). There is no LK key combination at power on (section 6.10). A broken U-Boot that cannot start Linux needs mtkclient on akos's laptop.

### 6.6 Picking the OS from a running system

- `reboot recovery` keeps working (LK, RTC bit 4), it skips U-Boot.
- For "reboot into LineageOS with modem" or "boot NixOS once", U-Boot needs a flag that survives a reset. Options:
  1. U-Boot environment in a partition, written with `fw_setenv` from NixOS and from LineageOS (root). Simple and standard (`ENV_IS_IN_MMC`).
  2. A magic word in reserved DRAM (the watchdog reset keeps DRAM). No storage writes, but lost on power off.
  3. RTC `PDN1`/`PDN2` spare bits. Needs pwrap in U-Boot.
- Recommendation: option 1, `boot_once=lineage` cleared by U-Boot after it reads it.

### 6.7 Extra effort on top of section 5

| Step | Effort |
|---|---|
| Keypad driver for `button_kbd` | 0.5 day |
| `bootmenu` entries, charger-mode bypass, env in a partition, `fw_setenv` config | 1 day |
| Android `boot.img` boot with `/firmware/android` fixup and `androidboot.*` from LK | 1 day |
| NixOS: extlinux on the `vendor` FAT partition, new bootloader module | 1 day |

Total with section 5: about 2 to 3 weeks of part-time work.

### 6.8 Decisions

1. LineageOS `boot.img`: on the `vendor` FAT partition (section 6.4).
2. The LineageOS 24.0 device tree builds a `recovery.img`. It is not tested yet.
3. Boot switching from a running OS: U-Boot env with `fw_setenv` (`boot_once`). The in-OS switcher is a later item.
4. Autoboot: NixOS after 3 s. Hold the side button to stop it.

### 6.9 Alternatives

- Patch LK: it is a closed binary, so every feature is reverse engineering work. U-Boot already has fastboot, menus, extlinux and Android images.
- lk2nd: Qualcomm only. No MediaTek port exists.

### 6.10 LK boot keys

- **[src]** LK disassembly (`re/lk.img`, function at `0x460030f4`, `mtk_detect_key()` at `0x46004c0c`). LK polls keys for about 50 ms before normal boot:
  - Key `0x11` (`MT65XX_BOOT_MENU_KEY`): LK shows "Select Boot Mode" with Recovery and Fastboot (`0x46004fb0`). `MT65XX_RECOVERY_KEY` is also `0x11`, so the menu always wins.
  - Key `0` (`MT65XX_FACTORY_KEY`): factory mode. LK skips this check after a PMIC reset.
- **[src]** `mtk_detect_key()` key codes: `8` is the PMIC power key (`TOPSTATUS` `0x24` bit 1), `0x11` is the PMIC home key (`TOPSTATUS` bit 3), other codes up to `0x47` are keypad bits (`KPD_MEM` at `0x10010004`, 16 bits per register, 0 = pressed).
- Test on the phone (2026-10-14, `getevent` on LineageOS with our kernel, every key pressed): the keypad sends 20 keys (digits, `*`, `#`, D-pad, `ENTER`, `MENU`, `BACK`, `SEND`, `F11`), the PMIC sends only `KEY_POWER`. No `KEY_HOME`, and keypad position (0, 0) is not used. So neither the boot menu key nor the factory key exists on this phone. LK has no reachable boot key.
- Consequence: the safe rollout is the only safety net.
  1. Test U-Boot from `recovery` (LK starts it only after `reboot recovery`). NixOS stays in `boot`.
  2. Move U-Boot to `boot` only when it starts NixOS by default, reliably, without any key press.
  3. U-Boot's own fastboot and a "LK fastboot" menu entry (RTC bit 13, needs pwrap) come before risky features.

## 7. Bring-up log

U-Boot tree: `u-boot/` (worktree of `ref/u-boot`, branch `agm-m7`). Build: `scripts/build-uboot.sh` (`out-uboot/boot.img`). Test: `scripts/test-uboot.sh` writes it to `recovery`, boots it with `reboot recovery`, and prints the U-Boot log from NixOS with `ramconsole`.

2026-10-14: U-Boot runs (banner, DRAM, LK DTB, watchdog reset back to NixOS). Facts found on the way, all **[src]** (tested on the phone):

- LK reads the last 512 KiB of the kernel blob (gzip + appended DTB) and scans backwards for the DTB magic. A blob smaller than 512 KiB gives a negative partition offset, and LK does not boot it. `build-uboot.sh` pads the gzip stream with zeros to 1 MiB.
- LK does not start a `boot.img` without a ramdisk. `build-uboot.sh` adds an empty cpio.
- Hand-off: AArch64 EL2, MMU and caches off, image at `0x40200000`, DTB in `x0` at `0x44000000`. `CNTFRQ_EL0` is 13 MHz.
- LK leaves the watchdog running (`WDT_MODE` `0x5d`: enabled, IRQ, dual mode, auto start). A U-Boot that stops or waits resets after about a minute, so U-Boot must service or reprogram the watchdog.
- In recovery mode the LK bootargs have `boot_reason=2 androidboot.bootreason=rtc`.
- `text_offset` in the image header does not matter here. U-Boot uses 0, like Linux.

2026-10-14, later: U-Boot services the watchdog (`mtk_wdt`, 15 s timeout; a 90 s `sleep` passes). MSDC0 works with the `mt8183-mmc` compat and fixed-clock stubs (LK leaves `msdc50_0_sel` at 384 MHz, `hclk` at 136.5 MHz): 8-bit HS52, GPT with all partition names. U-Boot boots the NixOS `boot.img` from `boot` with `bootm` (`CONFIG_ANDROID_BOOT_IMAGE`): `bootargs` = LK `/chosen/bootargs` from the `x0` DTB + the `boot.img` cmdline, DTB = the `x0` DTB. NixOS comes up; only the modem fails, because LK in recovery mode does not load it.

- `bootm` prints `ERROR: reserving fdt memory region failed ... -17` for 5 regions (MCUPM, SPM, framebuffer, ATF log, log store). LK puts them in the DTB twice, so LMB sees the second copy as existing. It does no harm.
- Display: `drivers/video/mt6739_dbi.c`, a port of the Linux `mt6739-dbi` driver. It reads the panel size from `DBI_ROI_SIZE`, points OVL layer 3 at a 32-bit XRGB framebuffer from the video uclass, and starts a DBI transfer in `video_sync`. `video_sync()` calls the driver before it flushes the cache, so the driver flushes the framebuffer itself, and it skips a sync while the DBI is busy. Linux takes the display over without problems.
- Keypad: `drivers/input/mtk_keypad.c` polls `KPD_MEM` and uses `linux,keymap`. Arrows, Enter and the digits work in `bootmenu` and on the console (`stdin=serial,keypad`).
- Boot partition (2026-10-14): `vendor` is FAT32, label `AGMBOOT` (backup in `backup/vendor-2026-10-14/`). `nixos/bootloader.nix` writes `/extlinux/extlinux.conf` and `/nixos/` with the stock extlinux builder (`-t -1 -r`, 8 generations), deletes the `MENU TITLE` line (any MENU line makes U-Boot ask) and shortens the labels to fit 30 columns. It still writes `boot.img` to `boot` while U-Boot is in `recovery`. LineageOS `boot.img` and `recovery.img` are in `/android/`.
- U-Boot menu: NixOS (`sysboot`, default after 3 s), NixOS generations (`sysboot -p`), LineageOS, LineageOS recovery, NixOS `boot.img`, Reboot. All of them boot. `ft_board_setup` puts the LK `/chosen/bootargs` before the OS arguments. `lk_bootmode` (third word of `/chosen/atag,boot`: 0 normal, 2 recovery, 8/9 charging) skips the menu in charger mode. The NixOS initrd needs `SUPPORT_RAW_INITRD`.
- LK already adds `/firmware/android` (from the ODM overlay); U-Boot only sets `boot_devices`.
- LineageOS recovery: ADB shows `unauthorized` until "Advanced > Enable ADB" in the recovery menu (D-pad works). Then `adb shell` is root. The recovery UI is far too large for 240 px.
- Fastboot (2026-10-14): MUSB with the `mt85xx` glue (`mediatek,mt6739-musb`, PHY base from the second `reg` entry, `0x11cc0800`). The MT6739 PHY zeroes the rest of the 32-bit word on a byte write, so the glue now does 32-bit read-modify-write (the old byte writes left `phy+6c` at `0x3e00` instead of `0x3f2f`, and the host saw nothing). `board_usb_init()` sets `usb_top_sel` to `univpll3_d4` and ungates `ICUSB`; LK had already done both. `vusb33` is on (`0x199c` = 1). Download speed about 8 MB/s (PIO). `fastboot flash <partition>` works for eMMC partitions by GPT name. pwrap works with the MT8365 register table (`pmic read`).
- LineageOS recovery cannot reboot itself to recovery (no `/misc` for the BCB); a plain reboot works.
- 2026-10-14: U-Boot moved to `boot`, LineageOS `recovery.img` to `recovery`. A normal boot now has `lk_bootmode` 0 and the modem works in NixOS. `scripts/test-uboot.sh` writes `recovery` and is no longer the right test loop: a broken U-Boot in `boot` can only be fixed from LineageOS recovery (`reboot recovery` from a running OS) or with mtkclient. Better loop for later: `fastboot boot` a test U-Boot from the U-Boot in `boot`.
- Graphical menu (2026-10-14): `agmmenu` (`board/mediatek/mt6739/menu.c`) replaces `bootmenu`. It draws straight into the framebuffer (filled rectangles, 8x16 and Terminus 16x32 glyphs) and waits for the DBI to be idle before each `video_sync`. Main menu grouped by OS (NixOS blue, full-width selection), 3 s countdown to NixOS; generations parsed from `extlinux.conf` and booted with `pxe_label_override`; fastboot screens (ready, progress with MB/s, OKAY/FAIL) through the new weak `fastboot_ui_update()` hook in the fastboot core. Back (keypad 5,3) is `KEY_ESC` in the U-Boot DT. While the menu runs, `stdout` is serial only, so U-Boot output does not draw over it.
- New U-Boot builds are tested from `recovery` (`reboot recovery`), then flashed to `boot` with U-Boot fastboot; LineageOS recovery goes back to `recovery` afterwards.
- LineageOS recovery UI (2026-10-14): device tree `recovery/res/images` has an 8x16 VGA font (`font.png`, `font_menu.png`, made with `scripts/recovery-font.py` from U-Boot's font) and the mdpi logo at 128x80. `bootable/recovery` branch `agm-m7` adds `ro.recovery.ui.menu_padding` and `ro.recovery.ui.compact` (8 px indent, 2 px item spacing, battery beside the logo); the device sets `compact=true`, `menu_padding=4`. Test loop: U-Boot Fastboot, then `fastboot boot recovery.img` (U-Boot's `fastboot_bootcmd` uses the LK DTB and `/firmware/android`).
- `/misc` is `para` (stock fstab: `by-name/para /misc emmc`). With it in our fstab, recovery clears the bootloader message, and `reboot recovery` from recovery works. `para` had a stale `bootonce-bootloader` from the stock system; LK ignored it.
- Host edits are sometimes not yet visible in the OrbStack VM when the build starts at once. `build-uboot.sh` then builds the old defconfig. Wait a second or check `out-uboot/.config`.

## Sources

- U-Boot: `drivers/usb/musb-new/mt85xx.c`, `musb_uboot.c`, `drivers/usb/gadget/udc/udc-uclass.c`, `drivers/phy/phy-mtk-tphy.c`, `drivers/mmc/mtk-sd.c`, `drivers/watchdog/mtk_wdt.c`, `drivers/power/pmic/mtk-pwrap.c`, `arch/arm/mach-mediatek/`, `configs/mt8516_pumpkin_defconfig`, `configs/mt8518_ap1_emmc_defconfig`, `arch/arm/lib/save_prev_bl_data.c`, `arch/arm/mach-snapdragon/board.c`, `doc/board/qualcomm/board.rst`, `doc/board/samsung/exynos-mobile*.rst`.
- U-Boot boot menu: `drivers/input/button_kbd.c`, `drivers/fastboot/fb_mmc.c`, `doc/usage/cmd/bootmenu.rst`, `doc/android/fastboot.rst`, `boot/bootmeth_extlinux.c`, `boot/image-android.c`.
- Our Linux tree: `arch/arm64/boot/dts/mediatek/mt6739.dtsi`, `drivers/usb/musb/mediatek.c`, `drivers/phy/mediatek/phy-mtk-tphy.c`, `drivers/mmc/host/mtk-sd.c`, `drivers/clk/mediatek/clk-mt6739.c`, `drivers/gpu/drm/tiny/mt6739-dbi.c`.
- Web: https://github.com/DavidBuchanan314/rabbit_r1_boot_notes, https://github.com/DavidBuchanan314/lk-mt6765, https://gitlab.com/mt6735-mainline, https://postmarketos.org/blog/2018/04/14/lowlevel/ (no U-Boot over MediaTek LK found).
