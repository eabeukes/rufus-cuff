# AGM M7 roadmap

Findings are in `NOTES.md`. This file tracks what is done and what is left.

## Done

- Boot: LK starts U-Boot from `boot` (menu, extlinux, fastboot; see `UBOOT-RESEARCH.md`), LineageOS recovery is in `recovery`, ODM overlay sink, pstore earlycon, early watchdog.
- Restart through the `mtk_wdt` restart handler.
- Clocks, pinctrl, GPIO, EINT.
- USB gadget: serial shell and ECM networking.
- eMMC (HS200). Access to the encrypted `userdata` (dm-crypt, f2fs), which NixOS no longer uses.
- postmarketOS rootfs from a loop image on `/data`.
- MT6357 PMIC: pwrap, regulators, RTC, auxadc.
- Keypad matrix and PMIC power key.
- Battery voltage from the auxadc.
- Power off through the PMIC.
- NixOS 26.05 userspace from a flake in this repo.
- Display: DRM driver for the DBI path that LK sets up, with fbcon.
- Battery: fuel gauge (current, coulomb counting, temperature), charger detection.
- I2C0 and I2C1, touchscreen, HL7005 charger.
- Flake builds the kernel (from GitHub) and `boot.img` parts. NixOS systemd initrd replaces the busybox initramfs.
- Graphical session: Sway (pixman) with `foot`, the `agm-menu` launcher and Firefox. The `wvkbd` on-screen keyboard stays hidden until the side key (F11) shows it.
- Fast test loop: deploy NixOS (`scripts/nixos-deploy.sh`), U-Boot writes nothing; reboot.
- Wi-Fi: CONSYS bring-up and the vendor gen2 driver in staging, with NetworkManager and iwd (see `WIFI.md`).
- GPU: vendor DDK 1.9 firmware on the mainline `powervr` driver, Mesa Vulkan 1.2, and GL 2.1 / GLES 2.0 through Zink (see `GPU.md`).
- T9 input method (`tools/t9`) with English and Polish word lists.
- Audio: own MT6739 AFE, `mt6357` codec, `aw8738` amplifier. Speaker, earpiece and main mic through PipeWire with a UCM profile.
- Modem (MD1, `MOLY_LR12A_R2_MP_V96_6`) as a data-only LTE modem: `mt6739-ccci` (power-on, handshakes, RPC, CLDMA, WWAN AT port and `wwan0`), `ccci-fsd` NVRAM service, ModemManager plugin `mtk-ccci`, NetworkManager connection `mobile` (IPv4, 57 Mbit/s down). Status bar, home screen and Settings show the mobile network. Reference: vendor `eccci` and `ccci_util` in `ref/lumi-mt6739`, mainline `t7xx`.
- microSD (UHS-I SDR104) as the NixOS root. `.#sd-image` with growing on the first boot (systemd-repart, `x-systemd.growfs`).
- Bootloader module: `switch-to-configuration` writes extlinux.conf and the generations to the FAT `vendor` partition for U-Boot.

## Follow-ups

- Modem: power-off and restart (driver and `state` attribute), so a crashed modem recovers without a phone reboot. ModemManager crashed the modem once (jump to 0) when it sent commands with the SIM still off.
- Modem: IPv6 bearer, CCB control setup, sequence check and status polling.
- microSD: test the install from `.#sd-image` on a fresh card.
- microSD: there is no card detect line, so a card is found only at boot.
- Delete the old root image `/data/nixos.img` on `userdata` (from LineageOS).
- USB host mode: VBUS boost in the HL7005 driver (`OPA_MODE`) and role switching through the WUSB3801. Host mode removes USB serial and networking while in use.
- Shrink the initrd (OpenSSL, libxml2, Kerberos and more are still in), so `boot.img` has more room in `boot`.
- Remove `regulator_ignore_unused` from the cmdline. First find which regulators must stay on.
- RTC: Linux reads the date as 2069-12-31. Check the year mapping against the stock driver (base year 1968).
- Offline charging: LK boots the `boot` partition (now NixOS) with the charger mode. Handle `androidboot.mode=charger` in NixOS (a small charging screen instead of the full session).
- auxadc: `batt_v` reads garbage and some channels time out.
- Charger: BC1.2 charger type detection for higher currents on wall chargers.
- Remove `clk_ignore_unused` from the cmdline.
- USB DMA (MUSB runs in PIO mode).
- eMMC HS400: find the strobe delay register in the MSDC0 pad macro (0x11cd0000) and give `mtk-sd` an MT6739 entry for it. A failed HS400 tuning leaves the eMMC unusable, so test with care. HS200 already gives most of the speed. The SD card had the same cause: its delay lines are in a second block (MSDC1 at 0x11c40000), so MSDC0 probably needs its block at 0x11cd0000 the same way. MSDC1 also lacks the 1.8 V TDSEL/RDSEL pad settings and the Schmitt trigger the vendor sets (see `SD-SDR104-RESEARCH.md`).
- Wi-Fi: CONSYS makes an EMI MPU write violation out of range at 0x40080400 at every boot (see `MODEM-RE.md` section 8).
- `MEMCG` crashes in `__refill_obj_stock` when systemd loads BPF.
- Display: panel power and init without LK, TE or a vblank event, DBI frame-done IRQ instead of polling.
- macOS sometimes does not create the USB network interface. A cable replug fixes it.
- T9: learn new words, number mode for number fields (`content_type`), keep `wvkbd` keys out of T9.
- GPU: HWR recovery after a lockup, GLES 3 (needs transform feedback), hardware WebRender in Firefox, Sway on the GPU instead of pixman. See `GPU.md`.
- Bluetooth, vibrator.
- Audio: after many fast manual profile switches, PipeWire once kept the old sink node while the device reported the other profile (no output route). A WirePlumber restart fixed it; find the cause if it comes back.
- Audio: check that the DAC/ADC clock gates really turn off when the DAPM supplies power down after the AFE has suspended.
- Power: system suspend (s2idle or deep, wake on keys, charger and RTC), the deeper stock idle states (SODI, deep idle; need the SPM firmware), EEM adaptive CPU voltages.
- Sway: find why the first `output * power on` after `power off` fails in wlroots, then drop the second call in the idle script.
- The `mt6357-regulator` VSRAM mask fix is a real mainline bug and could go upstream on its own.
