# MD1 modem power-on and first CCCI handshake (MT6739, MOLY_LR12A_R2_MP_V96_6)

Goal: a small mainline driver that turns on MD1, lets it run, and completes handshake 1 (HS1, `0x5555FFFF`) and handshake 2 (HS2, MD ready).

Sources and how sure each item is:
- **[S]** Vendor kernel `ref/lumi-mt6739` (`eccci/`, `eccci/mt6739/`, `ccci_util/`, `clk-mt6739-pg.c`, `pmic/mt6357/v1/pmic.c`, `spm_v4/`). It is close to the phone kernel, but not the same build. It has `MD_GENERATION 6293` (`eccci/mt6739/ccci_config.h`).
- **[L]** Stock LineageOS `logs/reference/wifi/dmesg.txt`. The stock kernel is 4.4.95, **32-bit ARMv7** (dmesg line 7). `CCCI_BOOTUP_LOG` lines go to a private buffer, so they are not in dmesg.
- **[B]** Stock DTB, decompiled from the stock `boot.img` (not in this repo).
- **[D]** LK disassembly (`~/agm-m7-backup/lk.img`, base 0x46000000, `scripts/re/lkdis.py`, `lkrefs.py` with capstone from `nix shell`).
- **[P]** Phone readout (values given in the task).
- **[M]** Mainline tree in `linux/`.

## 1. LK hand-off data

### 1.1 `/chosen/ccci,modem_info_v2` = `struct _ccci_lk_info_v2` [S] `ccci_util_lib_fo.c:75`
| off | field | [P] value | meaning |
|---|---|---|---|
| 0x00 | `u64 lk_info_base_addr` | 0x72000000 | Phys address of the LK tag area. It is the first bytes of the AP-MD shared memory. |
| 0x08 | `u32 lk_info_size` | 0xcd0 | Size of the tag area (3280 bytes). Max allowed is 0x10000. |
| 0x0c | `s32 lk_info_err_no` | 0 | LK load error. 0 = OK. |
| 0x10 | `s32 lk_info_version` | 2 | Tag format v2 (64-byte names). |
| 0x14 | `s32 lk_info_tag_num` | 0x1c | 28 tags. |
| 0x18 | `u32 lk_info_ld_flag` | 1 | MD1 loaded. The kernel does not read this field. |
| 0x1c | `s32 lk_info_ld_md_errno[4]` | 0,0,0,0 | Per-modem load error. |
| 0x2c | padding | 0 | `u64` alignment makes the struct 0x30 bytes. |

Tag v2 record (at `base + offset`): `char name[64]; u32 data_offset; u32 data_size; u32 next_tag_offset;` (76 bytes). Both offsets are relative to `lk_info_base_addr` [S] `find_ccci_tag_inf`.
Tag names in LK [D] (strings): `hdr_count`, `hdr_tbl_inf`, `retrieve_num`, `retrieve%d`, `free_in_kernel`, `md1_chk`, `md1img`, `smem_layout`, `ccb_info`, `ccb_gear_id`, `md1_smem_cahce_offset` (sic), `md1_phy_cap`, `md_mpu_num`, `md_mpu_inf`, `opt_*` (`opt_md1_support`, `opt_ps1_rat`, `opt_eccci_c2k`, ...).
Tag data the driver needs [S]:
- `hdr_tbl_inf`: `struct _modem_info { u64 base_addr; u32 size; s8 md_id; s8 errno; s8 md_type; s8 ver; u32 reserved[2]; }` ×`hdr_count`. Stock: MD1 0x62000000, size 0x7c50000 [L "ccci_md0 at LK"].
- `smem_layout`: `{ u64 base_addr; u32 ap_md1_smem_offset, ap_md1_smem_size, ap_md3_smem_offset, ap_md3_smem_size, md1_md3_smem_offset, md1_md3_smem_size, total_smem_size; }`. Stock: base 0x72000000, total 0x100000 [L].
- `ccb_info`: `{ u64 ccb_data_buffer_addr; u32 ccb_data_buffer_size; }` (LK inserts 16 bytes [D]). Expected 0x76000000 / 0xc00000 (= `mblock-9-ccci`).
- `md1_smem_cahce_offset`: LK writes 0x0e000000 [D] (224 MiB). The 6293 kernel hard-codes 224 MiB anyway [S] `ccci_md_config`.
- `md1_phy_cap`: PHY capture size. If 0, the AP reports `MD_PHY_CAPTURE` as not supported.
- `md1_chk`: the modem check header (`struct md_check_header*`). Optional for us.

**Important:** the tag area overlaps the smem region `MDCCCI_DBG` (offset 0). The vendor `ccci_util` zeroes the tag area after it parses it, and `md_cd_start` clears the smem on first boot [S]. The driver must parse the tags **before** it clears the shared memory.

### 1.2 `/chosen/atag,mdinfo` [S] `parse_meta_boot_arguments`
| word | [P] value | meaning |
|---|---|---|
| 0 | 0x00000003 | tag size in words (2 header + 1 data) |
| 1 | 0x41000806 | `ATAG_MDINFO_DATA` |
| 2 | 0x00000000 | bytes `[md_type, md_en, 0, 0]`. `md_en` = 0, so no META modem. The kernel uses the default. Stock logs "Using default META MD setting[0][0]" [L]. |
Meaning for us: normal (not META) boot. `booting_start_id` = 0 (section 4.3).

### 1.3 What LK already did (do not redo) [D]
- Loads `md1rom` and `md1dsp` to 0x62000000.
- **BANK0 remap** (MD view 0x00000000 = AP 0x62000000): INFRACFG_AO `BANK0_MAP0..3` at 0x10001300..0x1000130c. Two 16-bit slots per register, one slot per 32 MiB. Slot value = `((ap_addr >> 24) & 0x3fe) | 1` (bit 0 = enable).
- **BANK4 remap** (MD view 0x40000000 = AP 0x72000000, smem; MD view 0x4e000000 = AP `ccb_info` address): `BANK4_MAP0..3` at 0x10001320..0x1000132c, same slot format. The last slot gets the CCB buffer.
- **Remap lock**: 0x10001f80 |= BIT(16) ("before hw remap lock: MD1"). After this the kernel cannot change the remap.
- **EMI MPU** for all MD regions from `md_mpu_inf`, through SMC 0x82000209 (same call as CONSYS). The vendor kernel does no MPU setup on MT6739 (`SET_EMI_STEP_BY_STAGE` and `ENABLE_EMI_PROTECTION` are not defined) [S].
- LK does **not** power on or release MD1. LK code has only the MD1 **off** path (`[ccci-off]` at 0x4601c27c): if efuse word 4 bit 14 is set the MD is fused off; else it runs the vendor MTCMOS-off sequence, sets 0x10000000 |= 0x300 and clears 0x10001f0c[7:0]. LK calls it in recovery mode, and after the modem load when a flag at `[ctx+0xa8]` is set. So at kernel entry MD1 is off or in its reset default. The kernel must run the full power-on.

### 1.4 Memory map (AP phys / MD view) [S] `md1_6293_noncacheable_fat` + [L]
MD view of smem = `0x40000000 + ap - round_down(ap, 32 MiB)` = 0x40000000. MD view of CCB = `0x40000000 + 224 MiB + (ap - round_down(ap, 1 MiB))` = 0x4e000000.
| region | AP phys | MD view | size | note |
|---|---|---|---|---|
| MDCCCI_DBG | 0x72000000 | 0x40000000 | 2K | LK tags live here before clear |
| MDSS_DBG | 0x72000800 | 0x40000800 | 10K | |
| RUNTIME_DATA | 0x7200e800 | 0x4000e800 | 4K | AP rt data 0x800, then MD rt data 0x800 |
| FORCE_ASSERT | 0x7200f800 | 0x4000f800 | 1K | |
| DBM | 0x7200ff50 | 0x4000ff50 | 0xb0 | guard 0x44444444 ×2 at both ends |
| CCISM_SCP | 0x72010000 | 0x40010000 | 32K | not used |
| CCB_CTRL | 0x72018000 | 0x40018000 | 4K | [L] "remap control addr:0x72018000" |
| NETD / USB | 0x72019000 / 0x7201a000 | | 4K each | |
| AUDIO | 0x7201b000 | 0x4001b000 | 52K | [L] "smem_port->addr_phy=7201b000" |
| CCISM_MCU (CCIF ring buffers) | 0x72028000 | 0x40028000 | 705K, used 0xb0140 | section 3.3 |
| CCISM_MCU_EXP | 0x720d8400 | 0x400d8400 | 121K | exception ring buffers |
| CCB data (cacheable) | 0x76000000 | 0x4e000000 | 0xc00000 | CCB 2 MiB + DHL/MDM raw 10 MiB |
The stock kernel frees the unused parts of `mblock-8-ccci` (the `retrieve%d` tags and 0x69c50000-0x71ffffff) [L lines 498-509].

## 2. MD1 power-on sequence (vendor `md_cd_start` + `md_cd_power_on`) [S]
Bases: INFRACFG_AO 0x10001000, TOPCKGEN 0x10000000, APMIXED 0x1000c000, SPM 0x10006000, DVFSRC 0x10012000 [B]. MD-side registers are at 0x20000000+ and the AP reaches them only between steps 9 and 15.

| # | action | register / value | src |
|---|---|---|---|
| 0 | Vcore OPP0 request (`md_cd_vcore_config(1)`) | `vcorefs_request_dvfs_opp(KIR_APCCCI, OPP_0)`: Vcore 1.15 V, DDR 1344 | [S]+[L 38.1519] |
| 1 | First boot only: request IRQs, init CLDMA rings, clear smem (all regions except CCB_CTRL, AUDIO, PHY_CAP), then build CCIF ring buffers (section 3.3). Every boot: zero CCB data, zero first 1K of DHL raw. | | [S]+[L 38.244-38.295] |
| 2 | `ccci_set_clk_cg(1)`: enable infra CGs (write bit to CLR register) | CLDMA_BCLK: 0x100010a8 bit 3. CCIF_AP: 0x1000108c bit 23. CCIF_MD: bit 26. CCIF1_AP: bit 12. CCIF1_MD: bit 13. Status at 0x100010ac / 0x10001094. | [S] `clk-mt6739.c`+[B] |
| 3 | `md_ccif_sram_reset`: zero AP CCIF SRAM | 0x10209100..0x102092ff = 0 | [S]+[L 38.2962] |
| 4 | Reset the AP-side ring indices (EXP set, then NORMAL set) | smem only, no MMIO | [S]+[L] |
| 5 | `ccci_reset_ccif_hw` | INFRA_AO 0x10001150 \|= BIT(8) (assert), 0x10001154 \|= BIT(8) (release); then zero SRAM 0x100..0x2ff of AP CCIF and MD CCIF | [S] |
| 6 | `md_cldma_hw_reset` | 0x10001140 \|= BIT(6), 0x10001144 \|= BIT(6) (AO); 0x10001150 \|= BIT(2), 0x10001154 \|= BIT(2) (PD); 0x10001c00 \|= BIT(1) (CLDMA busy as wakeup) | [S] |
| 7 | MD clock mode | TOPCKGEN 0x10000000 &= ~0x300 | [S] (LK off path sets 0x300 [D]) |
| 8 | PMIC VMODEM (section 2.2) | MT6357 0x15aa[6:0] = 0x6f or 0x68 | [S]+[L 38.2963] |
| 9 | MTCMOS MD1 on (section 2.1) | `clk_prepare_enable("scp-sys-md1-main")` | [S]+[D] |
| 10 | `md1_pre_access_md_reg` | 0x10001370 &= ~BIT(0) (AP2MD dummy); 0x100012a8 = BIT(6) (MD->peri protect set); poll 0x10001258 bit 6 = 1 | [S] |
| 11 | DVFSRC MD srclkena | 0x10012018 \|= (3 << 26) | [S] `spm_v4/mtk_spm_vcorefs.c`; [L] shows 0x0c200000 |
| 12 | MD srclkena | 0x10001f0c = (val & ~0xff) \| 0x21 | [S] |
| 13 | `md1_pll_init` (section 2.3) | MD top PLL 0x20140000, CLKSW 0x20150000 | [S]; ~21 ms gap in [L] 38.2967 -> 38.3179 matches its `msleep(20)` |
| 14 | Disable MD WDT | MD RGU 0x200f0100 + 0x00 = 0x55000030 | [S] |
| 15 | `md_cd_let_md_go`: boot slave enable | 0x20000024 = 1 | [S] |
| 16 | `md1_post_access_md_reg` | 0x100012a0 = BIT(7) (AP->MD protect set), poll 0x10001228 bit 7 = 1; 0x100012ac = BIT(6) (MD->peri protect clear), poll 0x10001258 bit 6 = 0; 0x10001370 \|= BIT(0) | [S] |
| 17 | enable MD WDT IRQ and CCIF IRQs | | [S] |
| 18 | `cldma_reset`, `cldma_start` (section 3.2) | | [S]+[L 38.3179] |
| 19 | wait HS1 (section 4), then release Vcore OPP after HS2 | | [S]+[L] |

After step 16 the AP->MD bus is blocked. The AP must not touch 0x20000000+ after this, except in a debug dump that clears bit 7 again.
`md1_pcore_sram_on` is under `#if 0` [S]. `spm_ap_mdsrc_req` (SMC `MTK_SIP_KERNEL_SPM_AP_MDSRC_REQ`) is used only for PCCIF and debug dumps, not at boot [S].

### 2.1 MTCMOS MD1 on (`spm_mtcmos_ctrl_md1(STA_POWER_ON)`) [S] `clk-mt6739-pg.c:535` = [D] LK 0x4601c0ca
Precondition: SPM `POWERON_CONFIG_EN` 0x10006000 = 0x0b160001 (see CONSYS-RE.md).
| step | write / poll |
|---|---|
| 1 | `MD1_PWR_CON` 0x10006320 &= ~BIT(0) (RST_B) |
| 2 | \|= BIT(2) (PWR_ON), then \|= BIT(3) (PWR_ON_2ND) |
| 3 | poll `PWR_STATUS` 0x10006180 bit 0 = 1 and `PWR_STATUS_2ND` 0x10006184 bit 0 = 1 |
| 4 | 0x10006320 &= ~BIT(6) (SRAM_PDN; no ack poll for MD1) |
| 5 | `MD_SRAM_ISO_CON` 0x10006394 \|= BIT(0) |
| 6 | 0x10006320 &= ~BIT(1) (ISO), then &= ~BIT(5) (`MD_PWR_CLK_DIS`, not bit 4), then \|= BIT(0) (RST_B) |
| 7 | `MD_EXTRA_PWR_CON` 0x10006398 &= ~BIT(0) |
| 8 | `TOPAXI_PROTECTEN_1_CLR` 0x100012ac = BIT(6); `TOPAXI_PROTECTEN_CLR` 0x100012a4 = BIT(3)\|BIT(4)\|BIT(7) (0x98) |
| 9 | udelay(250) |
Off is the reverse: PROTECTEN_SET 0x100012a0 = 0x98, poll 0x10001224 & 0x98; PROTECTEN_1_SET 0x100012a8 = 0x40, poll 0x10001254 bit 6; 0x10006398 \|= 1; PWR_CON \|= BIT(5), \|= BIT(1); 0x10006394 &= ~1; PWR_CON \|= BIT(6), &= ~BIT(2), &= ~BIT(3); poll status bit 0 = 0; &= ~BIT(0) [S]+[D].
Mainline [M] `mt6739-pm-domains.h` has no MD1 domain. The generic scpsys code uses bit 4 for clock disable and needs SRAM acks, so MD1 does not fit as is. Write this sequence in the driver through the `scpsys` and `infracfg_ao` syscons, or add a custom domain.

### 2.2 PMIC (`vmd1_pmic_setting_on`, MT6357) [S] `pmic/mt6357/v1/pmic.c:40`
- Only VMODEM voltage is written. Enable bits are not touched. `RG_BUCK_VMODEM_VOSEL` = `BUCK_VMODEM_ELR0` 0x15aa[6:0]. Value: 0x6f (1.19375 V) if efuse word 28 bit 27 = 0, else 0x68 (1.15 V). Readback `DA_VMODEM_VOSEL` = 0x15a2. Enable `RG_BUCK_VMODEM_EN` = 0x1588 bit 0. Formula: 0.5 V + 6.25 mV × n.
- Vcore is only checked: `DA_VCORE_VOSEL` 0x1522 vs 0x65 (1.15 V) or 0x6d (1.20 V) by efuse word 28 bits [31:30]. [L] prints "vcore vosel = 0x65, da_vosel = 0x65". The source prints only on mismatch, so this phone's segment expects 0x6d, but Vcore is at 0x65 = OPP0 1.15 V. **Source and log disagree; the message is a harmless warning.** No VMODEM warning in [L], so VMODEM matched.
- [L 1.223] "vmodem: couldn't disable": the stock regulator core could not turn VMODEM off, so it stays on from boot. Mainline: `regulator_ignore_unused` is on the cmdline now. The driver must take `vmodem-supply` so VMODEM stays on.

### 2.3 `md1_pll_init` [S] (P = 0x20140000 MDTOP_PLLMIXED, C = 0x20150000 MDTOP_CLKSW, A = 0x1000c000)
1. Poll P+0x000 != 0 (PLL version), `msleep(20)` between reads.
2. A+0x000 (`AP_PLL_CON0`) \|= 0x2 (26 MHz low-pass filter), udelay(100).
3. P+0x60 = 0x80118000, P+0x58 = 0x800b8a00, P+0x50 = 0x80114e00, P+0x48 = 0x80114e00, P+0x40 = 0x800b8a00.
4. Poll until (P+0xc00 >> 14) & 1 = 0.
5. P+0x108 &= ~0x10000. P+0x14 = 0x10000, then 0x10001, udelay(100), 0x10000, 0x0.
6. P+0x108 \|= 0x10000, udelay(1). P+0x104 &= ~0x4000000, udelay(1).
7. P+0x10 = 0x100010; poll P+0x10 == 0x100010, `msleep(20)`, rewrite every 5th try.
8. Poll C+0x84 bit 15 = 1 (`msleep(20)`).
9. C+0x24 \|= 0x3, then \|= 0x058103fc. C+0x28 \|= 0x10. C+0x20 = 0x1.
10. P+0x314 = 0xffff, P+0x318 = 0xffff. P+0xf00 = 0x62930000 (marker "PLL done").

## 3. Hardware interface
### 3.1 Nodes [B] = [S] `mt6739.dts`
| block | base | IRQ (GIC SPI) | used by |
|---|---|---|---|
| CLDMA AO | 0x10014000 /0x1000 | 165, level high | net HIF |
| CLDMA PDN | 0x1021b000 /0x1000 | | net HIF |
| AP_CCIF0 | 0x10209000 /0x1000 | 148 (irq0), 149 (irq1), level low | control and all char ports |
| MD_CCIF0 | 0x1020a000 /0x1000 | (MD side) | reset/clear only |
| MD WDT | | 227, edge falling | |
| MD1 boot stats | 0x1020e300 (+0 stats0, +4 stats1) | | debug |
| mdcldmamisc | 0x1021c900 /0x400 | | debug dump only |
The `mdcldma` node lists IRQ 149 as "IRQ_CCIF1". It is the second line of AP_CCIF0, not the AP_CCIF1 block (0x1020b000, IRQ 150) [B]. The HIF requests 148 (`ap_ccif_irq0`, handler `md_ccif_isr`, ack `RCHNUM & 0xffff`). `modem_sys1` requests 149 (`ap_ccif_irq1`, handler `md_cd_ccif_isr`, ack `RCHNUM & 0x7fff8000` (`0xffff << 15`)) [S].

CCIF registers (same layout for AP and MD side) [S] `ccif_hif_platform.h`:
| off | name | use |
|---|---|---|
| 0x00 | CON | 0x01 = arbitration (written only on resume) |
| 0x04 | BUSY | AP writes `1 << ch` before send; skip send if bit already set |
| 0x08 | START | read only |
| 0x0c | TCHNUM | AP writes `ch` to raise the interrupt on the MD |
| 0x10 | RCHNUM | bitmap of channels from MD |
| 0x14 | ACK | write the bitmap to clear |
| 0x100 | CHDATA | 512-byte SRAM, shared by both sides |

CCIF channels for 6293 [S] (`RINGQ_BASE 0`, `RINGQ_SRAM 15`, `RINGQ_EXP_BASE 15`, 24 channels):
| ch | MD->AP | AP->MD |
|---|---|---|
| 0..6 | D2H_RINGQ0..6 (data in ring buffer n) | H2D_RINGQ0..6 |
| 7 | CCB wakeup (`AP_MD_CCB_WAKEUP`) | H2D_RINGQ7 (ring size 0) |
| 15 | D2H_SRAM (HS1 in SRAM) | H2D_SRAM (runtime data in SRAM) |
| 16 | D2H_EXCEPTION_INIT | H2D_EXCEPTION_ACK |
| 17 / 18 / 19 | EXCEPTION_INIT_DONE / CLEARQ_DONE / ALLQ_RESET | CLEARQ_ACK / FORCE_MD_ASSERT / MPU_FORCE_ASSERT |
| 20 / 21 | peer wakeup / `AP_MD_SEQ_ERROR` | |

### 3.2 CLDMA on 6293 [S]
Net only: 4 TX queues, 1 RX queue (`CLDMA_TXQ_NUM 4`, `CLDMA_RXQ_NUM 1`). `cldma_reset`: AO+0x418 (`DL_MTU_SIZE`) = 3584; AO+0x400 (`SO_CFG`) \|= 1. `cldma_start`: write GPD start addresses (PDN+0x000.. TX, AO+0x404 RX, plus 4-MSB registers), PDN+0x404 (`SO_START_CMD`) = all queues, unmask L2/L3 IRQs. Control traffic does not use CLDMA. The HS1/HS2 test can skip CLDMA (see section 7).

### 3.3 CCIF ring buffers in smem [S] `ccci_ringbuf.c` + [L]
Per queue, packed from CCISM_MCU (0x72028000): `u32 0xee0000ee ×2; struct { u32 read, write, length; } rx_control, tx_control; u8 rx[rx_size]; u8 tx[tx_size]; u32 0xff0000ff ×2;` (control part = 40 bytes).
| q | rx (MD->AP) | tx (AP->MD) | | q | rx | tx |
|---|---|---|---|---|---|---|
| 0 | 80K | 128K | | 4 | 20K | 20K |
| 1 | 80K | 40K | | 5 | 20K | 20K |
| 2 | 40K | 8K | | 6 | 64K | 64K |
| 3 | 80K | 40K | | 7 | 0 | 0 |
Total 0xb0140 bytes. [L] matches: "buf_size=213032 ... rx_size=81920, tx_size=131072, ctr_len=40". Exception set at 0x720d8400: rx/tx 12K, 32K, 8K, 0, 0, 0, 8K, 0. No flow-control block (`FLOW_CTRL_ENABLE` not set).
Packet in a ring: `u32 0xaabbaabb; u32 len;` then `len` bytes (16-byte `ccci_header` + payload), padded to 8 bytes, then `u32 0xccddeeff ×2`. To send on queue n: write in the tx area, update `tx_control.write`, then CCIF send channel n.

## 4. The first handshake
### 4.1 Common header [S] `mtk_ccci_common.h:77`
`struct ccci_header { u32 data[2]; u16 channel; u16 seq_num:15, assert_bit:1; u32 reserved; }` (16 bytes, packed).

### 4.2 HS1, MD -> AP [S]+[L]
MD writes into the CCIF SRAM and sends channel 15. Layout `struct ccif_sram_layout` at AP_CCIF0+0x100:
| AP_CCIF0 off | field | size |
|---|---|---|
| 0x100 | `dl_header` (MD->AP) | 16 |
| 0x110 | `md_rt_data` = `struct md_query_ap_feature` | 224 |
| 0x1f0 | `up_header` (AP->MD) | 16 |
| 0x200 | `ap_rt_data` | 220 (v2) or 156 (v2_1) |
[L] "dk_pkg=e2376100, md_featrue=e2376110" confirms 0x100 and 0x110.
`md_query_ap_feature` = `{ u32 head = 0x49434343; u8 feature_set[64]; u32 tail = 0x49434343; u8 reserved[152]; }`. Each `feature_set` byte: bits [3:0] `support_mask` (0 not exist, 1 not support, 2 must, 3 optional, 4 backward compat), bits [7:4] `version`.
The AP copies header + feature block into an skb and gives it to port `ccci_ctrl` (rx channel `CCCI_CONTROL_RX` = 0). [L] "control message 0x0,0x5555FFFF" prints `data[1]` = 0 (`MD_INIT_START_BOOT`) and `reserved` = `MD_INIT_CHK_ID` 0x5555ffff. That is HS1. HS1 came 58 ms after `cldma_start` [L 38.3179 -> 38.3755].
At HS1 the vendor also reads `md_boot_stats0/1` (0x1020e300/4, three reads, the third counts). [L]: 0x54430007 / 0x53320000.

### 4.3 AP answer, part 1: AP runtime data in smem [S] `ccci_md_prepare_runtime_data`
Check both patterns. Then for each id 0..63 write a record at RUNTIME_DATA (0x7200e800, max 0x800): `struct ccci_runtime_feature { u8 feature_id; u8 support_info; u8 reserved[2]; u32 data_len; u8 data[]; }`.
- If MD says 2 (must) and the AP list is below 2: fail (vendor gives up the boot).
- MD 0 or 2: copy the MD byte. MD 3: must + AP version if versions match and AP is must, else 1. MD 4: must if MD version >= AP version.
- Records with result 2 get data. All others get `data_len` = 0.
AP must-support list (6293): 0 BOOT_INFO, 1 EXCEPTION_SHM, 2 CCIF_SHM, 5 HIF_DMA_REMAP, 6 RTC_32K_LESS (only if no 32K crystal), 7 RANDOM_SEED, 8 GPS_COCLOCK, 9 SBP_ID, 10 MISC_CCCI, 11 CLIB_TIME, 12 C2K, 15 CCB_SHM, 16 DHL_RAW, 17 DT_NETD, 18 DT_USB, 19 EE_AFTER_EPOF, 20 AP_CCMNI_MTU, 25 LWA, 26 AUDIO_RAW, 27 MULTI_MD_MPU, 28 CCISM_EXP, 29 PHY_CAPTURE (only if size > 0), 30 MD_CONSYS. Optional: 13 MD_IMAGE_START. Not supported: 3, 4, 14, 21, 22, 24.
Data for each (all addresses are MD view):
| id | data |
|---|---|
| 0 | `{ boot_channel = 0, booting_start_id = 0 (normal, logging idle), boot_attributes = 0, boot_ready_id = 0 }` |
| 1 | `{ 0x40000000, 0x10000 }` |
| 2 | `{ 0x40028000, 0xb0140 }` (ring buffer total) |
| 15 | `{ 0x40018000, 0x1000, 0x4e000000, sum of CCB user sizes }` |
| 16 | `{ DHL raw MD addr, size }` (0x4e200000, 10 MiB) or `{0, 0}` |
| 17, 18, 26, 28 | `{ addr, size }` of NETD, USB, AUDIO, CCISM_EXP (size 0x1e400) |
| 25, 30 | `{ 0x40100000, 0 }` |
| 27 | none (`data_len` 0). Sets `multi_md_mpu_support`. |
| 5, 6, 8, 19 | 16 zero bytes |
| 7 | `{ random, 0, 0, 0 }` |
| 9 | `{ sbp_code, 0 or wm bitmap, 0, 0 }` |
| 10 | `{ BIT(0) seq check \| BIT(1) poll MD status, 0, 0, 0 }` |
| 11 | `{ tv_sec low, tv_sec high, tz minutes west, dst }` |
| 12 | `{ c2k flags (0 if no MD3 and no SVLTE/SRLTE), 0, 0, 0 }` |
| 13 | `{ 0x62000000, image size }` |
| 20 | `u32` 3568 (`NET_RX_BUF` - 16) |
Fields that can be zero for a first test: 5, 6, 8, 12, 19; CCB sizes if CCB is not used; 9 is 0 unless `md_cfg` sets an SBP code. Bit 0 of id 10 turns on per-channel sequence check (15-bit `seq_num`); for a first test set id 10 to 0 to avoid `AP_MD_SEQ_ERROR`.

### 4.4 AP answer, part 2: SRAM header + `ap_query_md_feature` [S] `md_cd_send_runtime_data_v2`, `ccif_hif_fill_rt_header`
1. `up_header` at 0x1f0: `data[0]` = 0, `data[1]` = packet size (16 + 156 = 0xac for v2_1, 16 + 220 = 0xec for v2), word 2 = `CCCI_CONTROL_TX` (1) with seq 0, `reserved` = 0x5555ffff.
2. `ap_rt_data` at 0x200 (zero it first). If `multi_md_mpu_support` (MD asked for id 27), use `ap_query_md_feature_v2_1`, else v2:
| field | v2_1 | v2 |
|---|---|---|
| head / tail | 0x43434349 | 0x43434349 |
| feature_set[64] | all 0, except [1] = 0x10 (version 1, mask 0) | same |
| share_memory_support | 2 (`MULTI_MD_MPU_SUPPORT`) | 1 (`INTERNAL_MODEM`) |
| ap_runtime_data addr / size | 0x4000e800 / 0x800 | same |
| md_runtime_data addr / size | 0x4000f000 / 0x800 | same |
| noncached MPU start / size | 0x40000000 / `ap_md1_smem_size` (0x100000) | `set_md_mpu_*` same values |
| cached MPU start / size | 0x4e000000 / CCB size (0xc00000) | (not in v2) |
| reserve_addr[12] / reserved[120] | 0 | 0 |
3. Write DBM guard (0x7200ff50: 0x44444444 ×2, 160 zero bytes, 0x44444444 ×2).
4. CCIF send channel 15: AP_CCIF0+0x04 = BIT(15), then +0x0c = 15.
[L] "send runtime data 0" at 38.3883.

### 4.5 HS2, MD -> AP [S]+[L]
MD sends a normal packet in ring buffer q0 (CCIF channel 0), channel `CCCI_CONTROL_RX` 0, `data[1]` = 0, `reserved` = 0. [L] "control message 0x0,0x0" at 38.7522, 364 ms after the runtime data. The FSM goes to READY (`md_state` 3 -> 4), then releases the Vcore OPP. HS2 needs the q0 ring header in smem and IRQ 148 working.
Other MD->AP control messages (`data[1]`): 4 MD_EX, 5 CCCI_DRV_VER_ERROR, 6 MD_EX_REC_OK, 8 MD_EX_PASS. Exception channels 16..19 come on CCIF too.

## 5. Ports for a data-only setup [S] `port/port_cfg.c` (6293: `MD1_NORMAL_HIF` = CCIF, `MD1_NET_HIF` = CLDMA)
| port | TX ch | RX ch | HIF | queue (normal) | queue (exception) | stock user [L] |
|---|---|---|---|---|---|---|
| ccci_ctrl | 1 | 0 | CCIF | 0 | 0 | kernel FSM |
| ccci_sys | 3 | 2 | CCIF | 0 | none | kernel |
| ccci_poll (status) | 68 | 67 | CCIF | 0 | 0 | kernel |
| ttyC0 (AT, MUX) | 12 | 10 | CCIF | 5 | none | gsm0710muxd |
| ccci_fs (NVRAM) | 15 | 14 | CCIF | 4 | 1 | ccci_fsd |
| ccci_rpc, ccci_rpc_k | 33 | 32 | CCIF | 1 | 1 | ccci_rpcd, kernel |
| ccci_md_log_ctrl | 8 | 6 | CCIF | 1 | 6 | (logger) |
| ttyC1 (MD log) | 43 | 42 | CCIF | 2 | 2 | |
| ccci_ipc_* | 36 | 34 | CCIF | 1 | none | ccci_mdinit (`ccci_ipc_5`) |
| ccmni0 | 22 | 20 | CLDMA | TX 0 / RX 0, ACK TX 1 | | rild data |
`ccci_fsd` starts before the MD start [L 38.1277 `ctl.start, ccci_fsd`]. The MD reads its NVRAM files through it after HS2 (expected; we did not check what the MD does without it). System messages that the AP must answer: `MD_GET_BATTERY_INFO` 0x105; others are callbacks [S] `port_sysmsg.c`.

## 6. Device-tree proposal (mainline)
```dts
modem@10014000 {
	compatible = "mediatek,mt6739-ccci";
	reg = <0 0x10014000 0 0x1000>,	/* CLDMA AO */
	      <0 0x1021b000 0 0x1000>,	/* CLDMA PDN */
	      <0 0x10209000 0 0x1000>,	/* AP CCIF0 */
	      <0 0x1020a000 0 0x1000>,	/* MD CCIF0 */
	      <0 0x1020e300 0 0x8>,	/* MD1 boot stats */
	      <0 0x20000000 0 0x1000>,	/* MD config: boot slave +0x24 */
	      <0 0x200f0000 0 0x1000>,	/* MD RGU, WDT at +0x100 */
	      <0 0x20140000 0 0x1000>,	/* MD top PLL */
	      <0 0x20150000 0 0x1000>;	/* MD clock switch */
	reg-names = "cldma-ao", "cldma-pdn", "ap-ccif", "md-ccif", "boot-stats",
		    "md-cfg", "md-rgu", "md-pll", "md-clksw";
	interrupts = <GIC_SPI 165 IRQ_TYPE_LEVEL_HIGH>,
		     <GIC_SPI 148 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 149 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 227 IRQ_TYPE_EDGE_FALLING>;
	interrupt-names = "cldma", "ccif0", "ccif1", "wdt";
	clocks = <&infracfg_ao CLK_INFRA_CLDMA_BCLK>,
		 <&infracfg_ao CLK_INFRA_CCIF_AP>, <&infracfg_ao CLK_INFRA_CCIF_MD>,
		 <&infracfg_ao CLK_INFRA_CCIF1_AP>, <&infracfg_ao CLK_INFRA_CCIF1_MD>;
	clock-names = "cldma", "ccif-ap", "ccif-md", "ccif1-ap", "ccif1-md";
	mediatek,infracfg = <&infracfg_ao>;	/* resets, bus protect, srclkena */
	mediatek,scpsys = <&scpsys>;		/* MD1 MTCMOS (section 2.1) */
	mediatek,topckgen = <&topckgen>;	/* +0x0 bits 9:8 */
	mediatek,apmixedsys = <&apmixedsys>;	/* AP_PLL_CON0 bit 1 */
	vmodem-supply = <&mt6357_vmodem_reg>;
};
```
- The clock IDs exist in mainline `mediatek,mt6739-clk.h` with the same numbers as stock (36, 37, 43, 46, 52) [M]+[B].
- No `memory-region`: LK creates the `mblock-*-ccci` nodes at run time, and the addresses come from the `/chosen` tags (section 1.1). The driver uses `memremap` on 0x72000000/0x100000 (nomap) and on the CCB buffer.
- CCIF reset (0x150 bit 8) and CLDMA PD reset (0x150 bit 2) are in a second reset bank. Mainline [M] registers only bank 0x140 (set 0x140, clear 0x144). Add bank 0x150 or write it through the syscon.
- DVFSRC (0x10012000) has no mainline node. Add a small reg entry or a syscon if step 11 is needed.

## 7. Open questions and risks
1. **MD feature request is unknown.** We do not know which ids MOLY V96_6 marks as must-support. The first spike must dump the 64 `feature_set` bytes from SRAM 0x110 and decide v2 vs v2_1 from id 27. If the MD requires CCB (15), DHL (16) or AUDIO (26), the AP must give real regions (zero sizes may be rejected).
2. **CLDMA before HS2.** The vendor starts CLDMA before HS1. It is not known if the MD checks CLDMA during boot. Plan: first try HS1/HS2 with CLDMA reset and clocks on but no queues. If HS2 does not come, add `cldma_start` with one empty RX GPD ring.
3. **Vcore / DVFSRC.** The vendor holds Vcore OPP0 (1.15 V) during boot and sets DVFSRC 0x10012018 bits 27:26. Mainline has no DVFSRC driver. Check the Vcore value at run time (MT6357 0x1522) before the start. Low Vcore can hang the MD silently.
4. **SPM key and secure state.** MTCMOS writes need 0x10006000 = 0x0b160001 (stock boot sets it). The remap and MPU are locked by LK (0x10001f80 bit 16, SMC). The kernel cannot change them, so the smem and CCB addresses must match LK. EMI MPU settings are secure-only (ATF SMC 0x82000209, stock caller was 32-bit). We do not need to call it.
5. **MD state at kernel entry.** LK may run the `[ccci-off]` power-down after load (flag at `[ctx+0xa8]`, not decoded). Our MTCMOS-on sequence starts with RST_B = 0, so it works from both states. Read 0x10006320 and 0x10006180 bit 0 first and log them.
6. **`mblock-8-ccci` is mapped.** The MD image area is in the linear map, but the EMI MPU forbids AP access to parts of it. Speculative reads can cause MPU violation reports. The stock 32-bit kernel had the same layout. Consider marking 0x62000000-0x69c4ffff `no-map` (and freeing the rest).
7. **`md_boot_stats`.** 0x1020e300 = 0x54430007, 0x1020e304 = 0x53320000 after HS1 [L]. The source has no decoder. `md_cd_dump_debug_register` treats 0x54430007, 0, and 0x5331xxxx as "boot stage" values. They look like ASCII stage codes ("TC", "S2"). Use them only as progress markers when HS1 does not come.
8. **Two CCIF IRQ lines.** We assume IRQ 148 fires for data and SRAM channels (0..15) and IRQ 149 for exception channels (16+). Both handlers read the same RCHNUM. Not verified; the driver can request both lines with one handler.
9. **Source vs phone.** `ref/lumi-mt6739` is not the phone kernel. The PLL magic values (section 2.3) and the `md_cd_power_on` order are not visible in [L] (private log). The ~21 ms gap in [L] matches the PLL `msleep(20)`. The vcore message (section 2.2) shows the efuse path differs from what the source expects. Other register values match [D] where LK has the same code (MTCMOS, srclkena, clock mode).
10. **VMODEM enable.** The vendor never sets `RG_BUCK_VMODEM_EN`. It is on from the PMIC default or LK. Mainline must keep it on (supply reference). Check 0x1588 bit 0 and 0x15a4 (`DA_VMODEM_EN`) before start.
11. **Vendor header oddity.** `modem_reg_base.h` defines both `CLDMA_AP_BASE` and `BASE_ADDR_MDRSTCTL` as 0x200f0000 (MD view names). The AP uses only `MD_RGU_BASE` 0x200f0100 for the WDT write.

## 8. Results on the phone
- 2026-10-12, spike driver `mt6739-ccci` (debugfs `start`): the power-on sequence of section 2 works as written. HS1 came 56 ms after the boot slave release, on IRQ SPI 148 (CCIF channel 15). Header: `data` 0xffffffff 0x0, channel 0, `reserved` 0x5555ffff. Both patterns 0x49434343. Boot stats 0x54430007 / 0x53320000, the same as stock [P].
- MD `feature_set`: id 0 = 0x02 (must, version 0). Ids 1..32 = 0x03 (optional, version 0). Ids 33..63 = 0 [P]. So the MD asks for id 27 (MULTI_MD_MPU) as optional, and the AP answer is `ap_query_md_feature_v2_1` (section 4.4).
- After HS1, MD1_PWR_CON = 0xd and PWR_STATUS bit 0 = 1 [P].
- The CCIF SRAM takes only 32-bit accesses. A byte write clears the other bytes of the word. `memcpy_toio` wrote the last 4 bytes of `ap_query_md_feature` byte by byte, so the tail pattern became 0x43000000 and the MD asserted 1.01 s later (channels 16, then 17+18, then 19). The MD wrote nothing to the smem in that case. Use 32-bit copies only [P].
- With the SRAM fix, the MD accepts the runtime data. 170 ms later it sends its first packet: queue 1, channel 32 (`ccci_rpc` RX), op 0x4005 (`IPC_RPC_GET_EINT_ATTR_OP`), 3 parameters: "MD1_SIM1_HOT_PLUG_EINT", 0x17, 6. Then it waits; no HS2 and no exception after 20 s [P]. The stock kernel found no such EINT in its DT ([L] "interrupts: not found") and answered with an error.
- Test settings: the whole smem is cleared before start (also CCB_CTRL and AUDIO), and `MISC_INFO_CCCI` (id 10) is 0 (no sequence check, no status polling).
- Unrelated: at every boot EMI_MPUS latches a CONN (domain 2) write out of range at 0x40080400 (EMI_MPUS 0x28400012, EMI_MPUT 0x40080400). This is the Wi-Fi CONSYS, not the modem. Clear with EMI+0x160.. = 0xffffffff, EMI+0x200.. = 3, EMI_MPUS = 0x80000000.
- RPC: the MD asks op 0x4005 for `MD1_SIM1..4_HOT_PLUG_EINT` (types 6, 0, 5) and op 0x400e for `mediatek,md_product_name_model_id`. Answering "not found" as stock (EINT: two params -11; DT query: 0 plus 68 bytes of 0x0f) is enough [P].
- Then the MD reads and writes its NVRAM through `ccci_fs` (channel 14/15, queue 4). See `CCCI-FS.md`; [ccci-fsd](https://github.com/itsvic-dev/ccci-fsd) serves it.
- With CCB (15) and DHL raw (16) offered, the MD asserts 3 s later in `ccci_shm_bm.c:571` (CCB buffer manager). Stock `ccci_mdinit` fills the CCB control area before the MD start. With both features marked not supported, the MD boots: HS2 (control message 0 0 on queue 0) 370 ms after the runtime data [P].
- After HS2: AT on channel 10/12 (queue 5, `ttyC0`, raw bytes, no user header) answers `ATI` with `MOLY.LR12A.R2.MP.V96.6`. Channel 4 (PCM/audio), 57/59 (IMS DL) and 169/171 (IMSM, WoA) also carry traffic; we ignore them.
- `AT+CFUN=1` gives SIM READY and 2G registration (the home network, CSQ 24). `+ERAT: 0,0,6,4` = current RAT GSM, mode 2G/3G/4G, LTE preferred.
- The board has no 32 kHz crystal: MT6357 RTC spare register 0x5b8 bit 6 is 0 (stock log: `rtc_spare_reg[12] = {5b8, 1, 6}`), so feature 6 (RTC_32K_LESS) must be offered as supported.
- `AT+CGATT=1` finds an LTE cell (`+EIRAT`, `+CEREG: 4,...,7`), then `CME ERROR: 100`. The MD raises channel 20 (peer wakeup) every 2 ms for 3.5 s and asserts in `errc_chm_main.c:8554` (params 0, 1, 0). All CCIF rings are drained at that time. Suspect: the AP CLDMA (network HIF) is not running.
- Exception info: `EX_OVERVIEW` at smem 0x800 (core names from 0x808, 12 bytes each), assert file at 0x870 (256 bytes), line and 3 params at 0x970.

## 9. LTE data (2026-10-12)
- With the AP CLDMA running (`CLDMA.md`), `AT+CGATT=1` attaches on LTE (`+COPS: 0,0,"<operator>",7`) and the MD does not assert. No ch 20 storm.
- Bearer setup, as the stock 6293 RIL does it (captured from LineageOS `logcat -b radio`, copy in `backup/partswap-2026-10-12/los-radio-boot.log`):
  1. `AT+EIFCONFIG=3`, `AT+EIAREG=1`, `AT+EDALLOW=1`
  2. `AT+EAPNSET="internet",1,"internet","internet"` and `AT+EAPNSET="internet",2,"type=default,supl,hipri;protocol=IP;roaming_protocol=IP;authtype=2;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;bearer_bitmask=2147352575;inactive_timer=0"`
  3. `AT+EIAAPN="internet",0` (initial attach APN)
  4. `AT+EAPNACT=1,"internet","default",0` gives `+EIF: 100, ifup, 1, <ipv4>, 0`, `+EIF: 100, mtu, 1500`, `+CGEV: ME PDN ACT 0`
  5. `AT+EPDN=0,"ifst",20` gives `+EPDN: 0, "new", 1, 100, 1500, 1, "<ipv4>"`
  6. `AT+EIF=100,"ifst","up",1` (the stock netagent confirms the interface; 1 = IPv4, 2 = IPv6)
- Interface id = 100 * n + ccmni index (100 ccmni0, 201 ccmni1, 302 ccmni2). The MD then sends RPC 0x400d `{net_if 0, lhif_q 2}`.
- Without steps 1 to 3, `AT+EAPNACT` and `AT+CGACT` fail (`CME ERROR: 5745`, `5873`, `5841`). Before step 6 the MD takes uplink packets but sends no downlink.
- Result: `ip addr add <ipv4>/32 dev wwan0`, ping 1.1.1.1 about 25 ms, 20 MB download at 4.6 MB/s (37 Mbit/s), no CLDMA errors.

## 10. LineageOS (2026-10-14)

Manual test on LineageOS 24.0 with our kernel, LK in normal mode (U-Boot in `boot`), static `ccci-fsd` from Nix (`pkgsStatic`), root ADB:

- Same steps as `nixos/modem.nix`: mount `protect1`/`protect2` read-only, seed a copy of `nvdata/md`, `md_cmn`, `protect_f/md`, `protect_s/md`, start `ccci-fsd /dev/ccci_fs <copy>`, write 1 to the driver `state`. `/dev/wwan0at0` and `wwan0` appear, the modem is `ready`.
- AT, in the order ModemManager uses on NixOS (trace in `logs/modem/nixos-mm-debug.log`): `ATE0`, `AT+CMEE=1`, `AT+EBOOT=0`, `AT+EFUN=0` (SIM on), `AT+CPIN?`, `AT+CFUN=1`, `AT+EIFCONFIG=3`, `AT+EIAREG=1` (CME 100, harmless), `AT+EDALLOW=1`, `AT+CGATT=1` (LTE attach), `AT+EIAAPN="internet",0`, `AT+EAPNACT=1,"internet","default",0`, `AT+EIF=<ifid>,"ifst","up",1`, `AT+EPDN=<aid>,"ifst",20`. Then the IPv4 address /32 on `wwan0`, a routing table for `wwan0`, and ping works.
- `AT+CGATT=1` while the modem is on GSM (`+CEREG` 4) blocked the AT channel two times (no answer, no URC, more than 10 minutes, only a reboot ends it). `AT+EDALLOW=1` sent while on GSM did the same one time. On 2026-10-15, `AT+CGATT=1` on GSM attached to LTE in a few seconds without a hang. The cause of the hang is not known. On NixOS a fresh copy of the stock NVRAM reaches LTE in 66 s, and ModemManager never sends `+CGATT`. The data call can start when `+CEREG` is 1 or 5. An earlier guess that the stock NVRAM lacks something was wrong; the stock APN table is already there (`+EAPNSET` gives `CME ERROR: 5841`).
- `adb push` of larger files broke the USB connection until `vm.min_free_kbytes` was raised to 16384, as on NixOS.
- Do not run one `cat` reader and `echo >>` markers into the same file: the reader overwrites from its own offset. Append from both sides (`>>`).

### Radio HAL (2026-10-15)

The AIDL radio HAL in `device/agm/m7/radio` (a copy of the goldfish HAL) gives mobile data on LTE with no manual steps. Findings:

- Packet attach: with `+EGTYPE: 0` (the default) the modem attaches for packet data only on `AT+CGATT=1`, and again after each `+CFUN=1`. Without it, it stays on GSM with `+CGREG`/`+CEREG` 4 and `+CGATT: 0`. `AT+EGTYPE=1` (always attach) makes it attach to LTE about 15 s after `+CFUN=1`. The HAL sends it at init.
- Radio off: `AT+CFUN=0` also turns the SIM off, and Android then shows "Invalid card" (`CARD_IO_ERROR`). The HAL uses `AT+CFUN=4`.
- `+EIFCONFIG=3`, `+EIAREG=1` and `+EDALLOW=1` go right after `+CFUN=1`, as ModemManager sends them.
- The data registration state comes from `+CGREG`/`+CEREG`, not `+CREG`. With `+CREG` the framework started data calls on GSM, every 10 s, and they all failed.
- The HAL does not send the initial attach APN (`+EIAAPN`) when the framework sets it. The modem attaches with the APN from its NVRAM. `setupDataCall` sends `+EAPNSET` and `+EIAAPN` before `+EAPNACT`, as ModemManager does.
- Not supported by this modem: `+CICCID` (use `AT+ICCID?`, the answer ends with an `F` pad), `+CGSN=2` (use `+CGSN`), `+CTEC` (emulated from the last `<AcT>`), `+CLCK="FD"`, `+CMOD`, `+CMUT`, `+CSCB`. Unsupported commands answer a plain `ERROR`.
- Answer formats: CR LF line ends; raw answers have CRs around them; `+CRSM: 144, 0, "<hex>"` has spaces and quotes; `+COPS: 0` without an operator before registration.
- APN database: a `userdata` that was made with a Wi-Fi only build has an empty APN table (1 row), and the framework says `NO_SUITABLE_DATA_PROFILE`. `content delete --uri content://telephony/carriers/restore` loads `apns-conf.xml`. A clean install does not need it.
