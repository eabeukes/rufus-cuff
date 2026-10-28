# Rufus Cuff hardware notes

Raw data: `dt.tar` (the `/sys/firmware/devicetree/base` tree) and `live.dtb` (`/sys/firmware/fdt`),
both pulled 2026-10-27 from the running stock kernel, after LK filled in `/chosen`.
Stock kernel config: `../kernel-4.4.22.config`.

## Board

- Project `C10_DW_480X640_TFT` (same as the DM100), Wiite software stack, RC088 eng `boot`/`recovery`/`lk`.
- MT6739WA, 4x Cortex-A53 up to 1274 MHz, 3 GiB RAM, 32 GB eMMC. PMIC MT6357 (`mt6357_charger`, `mt6357_gauge`).
- Stock kernel: 32-bit, 4.4.22, `bootopt=64S3,32S1,32S1`, `buildvariant=eng`.

## Memory map (stock)

| Region | Address | Size | Use |
|---|---|---|---|
| System RAM | `0x40000000` | 3 GiB, with holes | |
| `ram_console` | `0x54400000` | `0x10000` | MTK ram console |
| `pstore` (`mediatek,pstore`) | `0x54410000` | `0xe0000` | Android shows `console-ramoops`, `console-ramoops-2`, `pmsg-ramoops-0`. No record/console size properties in the DT; the vendor driver hard-codes them |
| `atf-reserved` | `0x54600000` | `0x40000` | ATF |
| LK framebuffer (`mblock-5-framebuffer`) | `0x7fb10000` | `0x470000` | |

## Display

- LK's `atag,videolfb` struct (little-endian): fb base `0x7fb10000`, LCM found = 1, fps 77.87 (`0x1e6b`/100), vram `0x470000`, name `lk_st7701S_V2_480X640_dsi_vdo`.
- Panel: Sitronix ST7701S, 480x640, **DSI video mode**. Android: 480x640 at 197 dpi.
- Ignore the separate `atag,videolfb-*` properties (`lcmname=rm67160_400x400_dsi_cmd_oled`, `fb_base_l=0x5e605000`). They are DTB defaults LK doesn't overwrite.
- DT nodes: `dsi0@14012000`, `mipi_tx0@11c80000`, `disp_ovl0@1400a000`, `disp_rdma0@1400b000`, `disp_pwm0@1100e000`, `dsi_te`; backlight candidates `aw_bl@0`, `wiite_bl@0`, `led@*`.

## I2C

| Bus-addr | Name | Stock driver |
|---|---|---|
| 0-003e | `i2c_lcd_bias` | |
| 0-005d | `cap_touch` | `hyn_ts` (Hynitron, `CONFIG_TOUCHSCREEN_MTK_CTS0XX`) |
| 1-000c | `msensor` | |
| 1-0033 | `heartrate` | `heartrate` (`wiite-hrs`) |
| 1-004c | `gsensor` | `bma4xy_acc` |
| 1-0068 | `gyro` | |
| 1-006a | `sw_charger` | |
| 1-0077 | `barometer` | |
| 3-0008 | `nfc` | |

## Input

- `mtk-kpd` advertises KEY 0x66 HOME, 0x6b END, 0x72 VOLDOWN, 0x73 VOLUP, 0x74 POWER, 0x8b MENU, 0x9e BACK, 0xd3, 0x14a. Physical buttons: TBD.
- `ACCDET` (headset detect), `mtk-tpd` (touch).

## Other

- Wi-Fi/BT: `CONSYS_MT6739`, firmware in `/system/vendor/firmware` (`ROMv2_lm_patch_1_*_hdr.bin`, `WIFI_RAM_CODE_6755`, `WMT_SOC.cfg`).
- Nordic co-processor nodes: `wiite_nrf@0`, `nrf_ctrl_nwk@0`, `nrf_ctrl_wk@0`.
- `mmc1` host exists but is empty (no card slot found).
- `/data` is unencrypted ext4 on `mmcblk0p31`.
