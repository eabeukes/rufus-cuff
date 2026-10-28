// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6357 PMIC charger input detection and fuel gauge, and (with "rufus,charge-control")
 * the PMIC's linear charger, for boards without an external charger (Rufus Cuff).
 */

#include <linux/bitfield.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CHR_TOP_CON0_RGS_CHRDET		BIT(4)

/*
 * Linear charger fields, from MediaTek's mt6357_charger.c / mt6357-pulse-charger.h.
 * Checked against the stock kernel's values while charging (1000 mA, 4.2 V):
 * CHR_CON0 0xfb6, CHR_CON1 0x1f84, CHR_TOP_CON3 0x234e, CHR_TOP_CON5 0x205.
 */
#define CHR_TOP_CON3_VCDT_MODE		BIT(0)
#define CHR_TOP_CON3_VCDT_HV_VTH	GENMASK(10, 6)
#define CHR_TOP_CON3_VBAT_OV_VTH	GENMASK(13, 11)
#define CHR_TOP_CON5_VBAT_CV_VTH	GENMASK(4, 0)
#define CHR_CON0_CS_VTH			GENMASK(3, 0)
#define CHR_CON0_CS_EN			BIT(4)
#define CHR_CON0_VBAT_OV_EN		BIT(5)
#define CHR_CON0_VBAT_CV_EN		BIT(7)
#define CHR_CON0_VCDT_HV_EN		BIT(11)
#define CHR_CON1_CSDAC_MODE		BIT(7)
#define CHR_CON1_HWCV_EN		BIT(9)
#define CHR_CON1_ULC_DET_EN		BIT(10)
#define CHR_CON1_CSDAC_EN		BIT(11)
#define CHR_CON1_CHR_EN			BIT(12)
#define CHR_CON9_CHRWDT_TD		GENMASK(5, 2)
#define CHR_CON9_CHRWDT_EN		BIT(6)
#define CHR_CON9_CHRWDT_WR		BIT(7)

#define CHRWDT_TD_32S			3
#define VBAT_OV_VTH_4V45		4
#define VCDT_HV_VTH_7V			0xd

/* Never charge above this, whatever the DT says. */
#define CHARGE_CV_MAX_UV		4200000
#define CHARGE_TEMP_MIN_DECIC		0
#define CHARGE_TEMP_MAX_DECIC		450
/* Full: within 50 mV of CV and below the termination current for 3 polls (30 s). */
#define FULL_DROP_UV			50000
#define FULL_POLLS			3
#define RECHARGE_DROP_UV		100000
#define CHARGE_TEMP_HYST_DECIC		30
#define CHARGE_OV_MARGIN_UV		50000
#define CHARGE_TIME_MAX			(8 * 3600 * HZ)

/* RG_CS_VTH: charge current per register value. */
static const u32 cs_vth_ua[] = {
	2000000, 1600000, 1500000, 1350000, 1200000, 1100000, 1000000, 900000,
	800000, 700000, 650000, 550000, 450000, 300000, 200000, 70000,
};

/* RG_VBAT_CV_VTH: the values up to 4.2 V (higher ones are never used). */
static const u32 cv_vth_uv[] = {
	3900000, 4000000, 4050000, 4100000, 4150000, 4200000,
};

#define FGADC_CON1_SW_CTRL		GENMASK(3, 0)
#define FGADC_CON1_SW_READ_PRE		1
#define FGADC_CON1_SW_CLEAR		8
#define FGADC_CON1_LATCHDATA_ST		BIT(15)

/* nA per FG_CURRENT LSB with a 10 mOhm sense resistor. */
#define FG_CURRENT_UNIT_NA		305176
/* Stock CAR_TUNE_VALUE. */
#define FG_CAR_TUNE_PERMILLE		930
/* Coulomb counter: 10.85 uAh per 2^11 LSB with a 10 mOhm sense resistor. */
#define FG_CAR_UNIT_NAH			10850
#define FG_CAR_SHIFT			11
/* Battery and sense resistance, to estimate OCV under load. */
#define BAT_R_MOHM			150

#define POLL_INTERVAL			(10 * HZ)
#define FULL_CURRENT_UA			50000

/* NTC 10 kOhm with 16.9 kOhm pull-up to 1.8 V. */
#define NTC_PULL_UP_OHM			16900
#define NTC_PULL_UP_MV			1800

static const struct {
	int decic;
	int ohm;
} ntc_table[] = {
	{ -400, 195652 }, { -350, 148171 }, { -300, 113347 }, { -250, 87559 },
	{ -200, 68237 }, { -150, 53650 }, { -100, 42506 }, { -50, 33892 },
	{ 0, 27219 }, { 50, 22021 }, { 100, 17926 }, { 150, 14674 },
	{ 200, 12081 }, { 250, 10000 }, { 300, 8315 }, { 350, 6948 },
	{ 400, 5834 }, { 450, 4917 }, { 500, 4161 }, { 550, 3535 },
	{ 600, 3014 },
};

struct mt6357_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *bat;
	struct power_supply_battery_info *info;
	struct iio_channel *vbat;
	struct iio_channel *temp;
	struct delayed_work work;
	struct mutex lock;
	bool online;
	int vbat_uv;
	int current_ua;
	int temp_decic;
	bool gauge_ready;
	s64 car0_uah;
	int charge0_uah;
	int charge_uah;
	/* Linear charger control. */
	bool charge_ctl;
	bool charging;
	bool full;
	bool fault;
	int full_polls;
	bool timed_out;
	bool stopping;
	unsigned long online_since;
	int irq;
	unsigned int cv_sel;
	unsigned int cs_sel;
};

static const enum power_supply_property mt6357_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const enum power_supply_property mt6357_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
};

static bool mt6357_charger_online(struct mt6357_charger *chg)
{
	unsigned int con;

	if (regmap_read(chg->regmap, MT6357_CHR_TOP_CON0, &con))
		return false;
	return con & CHR_TOP_CON0_RGS_CHRDET;
}

static int mt6357_read_temp(struct mt6357_charger *chg, int *decic)
{
	int mv, ohm, i, ret;

	ret = iio_read_channel_processed(chg->temp, &mv);
	if (ret < 0)
		return ret;
	if (mv <= 0 || mv >= NTC_PULL_UP_MV)
		return -ERANGE;

	ohm = NTC_PULL_UP_OHM * mv / (NTC_PULL_UP_MV - mv);
	if (ohm >= ntc_table[0].ohm || ohm <= ntc_table[ARRAY_SIZE(ntc_table) - 1].ohm)
		return -ERANGE;

	for (i = 1; ohm < ntc_table[i].ohm; i++)
		;
	*decic = ntc_table[i].decic - (ohm - ntc_table[i].ohm) *
		 (ntc_table[i].decic - ntc_table[i - 1].decic) /
		 (ntc_table[i - 1].ohm - ntc_table[i].ohm);
	return 0;
}

static int mt6357_fg_latch_read(struct mt6357_charger *chg, unsigned int reg,
				unsigned int count, unsigned int *val)
{
	unsigned int sta, i;
	int ret;

	ret = regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL,
				 FGADC_CON1_SW_READ_PRE);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(chg->regmap, MT6357_FGADC_CON1, sta,
				       sta & FGADC_CON1_LATCHDATA_ST, 100, 10000);
	for (i = 0; !ret && i < count; i++)
		ret = regmap_read(chg->regmap, reg + 2 * i, &val[i]);

	regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL,
			   FGADC_CON1_SW_CLEAR);
	regmap_read_poll_timeout(chg->regmap, MT6357_FGADC_CON1, sta,
				 !(sta & FGADC_CON1_LATCHDATA_ST), 100, 10000);
	regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL, 0);
	return ret;
}

static int mt6357_read_fg_current(struct mt6357_charger *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = mt6357_fg_latch_read(chg, MT6357_FGADC_CUR_CON0, 1, &val);
	if (ret)
		return ret;

	*ua = div_s64((s64)(s16)val * FG_CURRENT_UNIT_NA * FG_CAR_TUNE_PERMILLE,
		      1000 * 1000);
	return 0;
}

static int mt6357_read_fg_charge(struct mt6357_charger *chg, s64 *uah)
{
	unsigned int val[2];
	s32 car;
	int ret;

	ret = mt6357_fg_latch_read(chg, MT6357_FGADC_CAR_CON0, 2, val);
	if (ret)
		return ret;

	car = (s32)(val[1] << 16 | val[0]);
	*uah = div_s64((s64)car * FG_CAR_UNIT_NAH * FG_CAR_TUNE_PERMILLE,
		       (1000LL * 1000) << FG_CAR_SHIFT);
	return 0;
}

static int mt6357_battery_status(struct mt6357_charger *chg)
{
	if (!chg->online)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (chg->full)
		return POWER_SUPPLY_STATUS_FULL;
	if (chg->current_ua > FULL_CURRENT_UA)
		return POWER_SUPPLY_STATUS_CHARGING;
	if (chg->current_ua > -FULL_CURRENT_UA &&
	    chg->vbat_uv >= chg->info->constant_charge_voltage_max_uv - 50000)
		return POWER_SUPPLY_STATUS_FULL;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static void mt6357_gauge_update(struct mt6357_charger *chg)
{
	int full = chg->info->charge_full_design_uah;
	int ocv, cap;
	s64 car;

	if (mt6357_read_fg_charge(chg, &car))
		return;

	if (!chg->gauge_ready) {
		ocv = chg->vbat_uv - chg->current_ua / 1000 * BAT_R_MOHM;
		cap = power_supply_batinfo_ocv2cap(chg->info, ocv, 25);
		if (cap < 0)
			return;
		chg->charge0_uah = div_s64((s64)full * cap, 100);
		chg->car0_uah = car;
		chg->gauge_ready = true;
	}

	if (mt6357_battery_status(chg) == POWER_SUPPLY_STATUS_FULL) {
		chg->charge0_uah = full;
		chg->car0_uah = car;
	}

	chg->charge_uah = clamp_t(s64, chg->charge0_uah + car - chg->car0_uah, 0, full);
}

/* Program CV and current, check they stuck, then switch the charger on (as the vendor driver). */
static int mt6357_charge_enable(struct mt6357_charger *chg)
{
	struct regmap *map = chg->regmap;
	unsigned int top5, con0;
	int ret;

	ret = regmap_update_bits(map, MT6357_CHR_TOP_CON5, CHR_TOP_CON5_VBAT_CV_VTH,
				 FIELD_PREP(CHR_TOP_CON5_VBAT_CV_VTH, chg->cv_sel));
	ret = ret ?: regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_CS_VTH,
					FIELD_PREP(CHR_CON0_CS_VTH, chg->cs_sel));
	ret = ret ?: regmap_read(map, MT6357_CHR_TOP_CON5, &top5);
	ret = ret ?: regmap_read(map, MT6357_CHR_CON0, &con0);
	if (ret)
		return ret;
	if (FIELD_GET(CHR_TOP_CON5_VBAT_CV_VTH, top5) != chg->cv_sel ||
	    FIELD_GET(CHR_CON0_CS_VTH, con0) != chg->cs_sel) {
		dev_err(chg->dev, "charger settings did not stick (0x%x 0x%x), charging off\n",
			top5, con0);
		chg->fault = true;
		return -EIO;
	}

	ret = regmap_set_bits(map, MT6357_CHR_CON0, CHR_CON0_CS_EN | CHR_CON0_VBAT_CV_EN |
			      CHR_CON0_VBAT_OV_EN | CHR_CON0_VCDT_HV_EN);
	ret = ret ?: regmap_set_bits(map, MT6357_CHR_CON1, CHR_CON1_CSDAC_MODE |
				     CHR_CON1_HWCV_EN | CHR_CON1_ULC_DET_EN | CHR_CON1_CSDAC_EN);
	return ret ?: regmap_set_bits(map, MT6357_CHR_CON1, CHR_CON1_CHR_EN);
}

static void mt6357_charge_disable(struct mt6357_charger *chg)
{
	regmap_clear_bits(chg->regmap, MT6357_CHR_CON1, CHR_CON1_CHR_EN);
}

/* Test knob: with kick_wdt=0 the poll stops kicking, to check the watchdog stops charging. */
static bool kick_wdt = true;
module_param(kick_wdt, bool, 0644);

/*
 * Called every poll. The charger watchdog (32 s) is kicked here, so charging stops by
 * itself if this stops running. Charges only with a charger, a battery temperature
 * reading inside 0-45 C (restart below 42 C / above 3 C), for at most 8 hours per plug-in,
 * and until the current drops below the termination current at CV. Above CV + 50 mV
 * it stops for good (until reboot).
 */
static void mt6357_charge_update(struct mt6357_charger *chg, bool vbat_ok)
{
	int cv = cv_vth_uv[chg->cv_sel];
	int term = chg->info->charge_term_current_ua > 0 ?
		   chg->info->charge_term_current_ua : 150000;
	int hyst = chg->charging ? 0 : CHARGE_TEMP_HYST_DECIC;
	bool temp_ok = chg->temp_decic != INT_MIN &&
		       chg->temp_decic >= CHARGE_TEMP_MIN_DECIC + hyst &&
		       chg->temp_decic <= CHARGE_TEMP_MAX_DECIC - hyst;
	bool want;

	if (vbat_ok && chg->vbat_uv > cv + CHARGE_OV_MARGIN_UV && !chg->fault) {
		dev_err(chg->dev, "battery at %d mV, above %d mV: charging off until reboot\n",
			chg->vbat_uv / 1000, cv / 1000);
		chg->fault = true;
	}
	if (chg->charging && !chg->timed_out &&
	    time_after(jiffies, chg->online_since + CHARGE_TIME_MAX)) {
		dev_warn(chg->dev, "charging for 8 h: stopped until the charger is replugged\n");
		chg->timed_out = true;
	}

	if (!chg->online) {
		chg->full = false;
		chg->full_polls = 0;
		chg->timed_out = false;
	} else if (chg->charging && vbat_ok && chg->vbat_uv >= cv - FULL_DROP_UV &&
		   chg->current_ua < term) {
		if (++chg->full_polls >= FULL_POLLS)
			chg->full = true;
	} else {
		chg->full_polls = 0;
		if (chg->full && vbat_ok && chg->vbat_uv < cv - RECHARGE_DROP_UV)
			chg->full = false;
	}

	want = chg->online && vbat_ok && temp_ok && !chg->full && !chg->fault && !chg->timed_out;
	if (want != chg->charging)
		dev_info(chg->dev, "charging %s (%d mV, %d mA, %d.%d C%s)\n",
			 want ? "on" : "off", chg->vbat_uv / 1000, chg->current_ua / 1000,
			 chg->temp_decic / 10, abs(chg->temp_decic % 10),
			 chg->full ? ", full" : "");

	if (kick_wdt)
		regmap_write_bits(chg->regmap, MT6357_CHR_CON9, CHR_CON9_CHRWDT_WR,
				  CHR_CON9_CHRWDT_WR);
	if (want && mt6357_charge_enable(chg))
		want = false;
	if (!want)
		mt6357_charge_disable(chg);
	chg->charging = want;
}

/* Hand the charger back as found under the stock kernel: off, watchdog off. */
static void mt6357_charge_release(void *data)
{
	struct mt6357_charger *chg = data;

	mt6357_charge_disable(chg);
	regmap_clear_bits(chg->regmap, MT6357_CHR_CON9, CHR_CON9_CHRWDT_EN);
}

static int mt6357_charge_init(struct mt6357_charger *chg)
{
	struct device *dev = chg->dev;
	struct regmap *map = chg->regmap;
	u32 cv = min_t(u32, chg->info->constant_charge_voltage_max_uv, CHARGE_CV_MAX_UV);
	int cc = chg->info->constant_charge_current_max_ua;
	unsigned int con0, con1, con6, con9, top3, top5;
	int i, ret;

	if (cc <= 0)
		return dev_err_probe(dev, -EINVAL, "battery needs a charge current\n");
	for (i = ARRAY_SIZE(cv_vth_uv) - 1; i > 0 && cv_vth_uv[i] > cv; i--)
		;
	chg->cv_sel = i;
	for (i = 0; i < ARRAY_SIZE(cs_vth_ua) - 1 && cs_vth_ua[i] > cc; i++)
		;
	chg->cs_sel = i;

	if (!regmap_read(map, MT6357_CHR_CON0, &con0) && !regmap_read(map, MT6357_CHR_CON1, &con1) &&
	    !regmap_read(map, MT6357_CHR_CON9, &con9) && !regmap_read(map, MT6357_CHR_TOP_CON3, &top3) &&
	    !regmap_read(map, MT6357_CHR_TOP_CON5, &top5) && !regmap_read(map, MT6357_CHR_CON6, &con6))
		dev_info(dev, "found CON0 0x%x CON1 0x%x CON6 0x%x CON9 0x%x TOP3 0x%x TOP5 0x%x\n",
			 con0, con1, con6, con9, top3, top5);

	/* Vendor init: 32 s watchdog, 4.45 V battery over-voltage, 7 V VCDT, CC mode. */
	ret = regmap_update_bits(map, MT6357_CHR_CON9, CHR_CON9_CHRWDT_TD | CHR_CON9_CHRWDT_EN,
				 FIELD_PREP(CHR_CON9_CHRWDT_TD, CHRWDT_TD_32S) | CHR_CON9_CHRWDT_EN);
	ret = ret ?: regmap_write_bits(map, MT6357_CHR_CON9, CHR_CON9_CHRWDT_WR, CHR_CON9_CHRWDT_WR);
	ret = ret ?: regmap_update_bits(map, MT6357_CHR_TOP_CON3, CHR_TOP_CON3_VCDT_MODE |
					CHR_TOP_CON3_VCDT_HV_VTH | CHR_TOP_CON3_VBAT_OV_VTH,
					FIELD_PREP(CHR_TOP_CON3_VCDT_HV_VTH, VCDT_HV_VTH_7V) |
					FIELD_PREP(CHR_TOP_CON3_VBAT_OV_VTH, VBAT_OV_VTH_4V45));
	ret = ret ?: regmap_set_bits(map, MT6357_CHR_CON0, CHR_CON0_VBAT_OV_EN | CHR_CON0_VCDT_HV_EN);
	ret = ret ?: regmap_set_bits(map, MT6357_CHR_CON1, CHR_CON1_CSDAC_MODE | CHR_CON1_ULC_DET_EN);
	if (ret)
		return dev_err_probe(dev, ret, "charger init failed\n");

	dev_info(dev, "charge control: %u mV, %u mA\n", cv_vth_uv[chg->cv_sel] / 1000,
		 cs_vth_ua[chg->cs_sel] / 1000);
	/* Off until the first poll has checked the charger, battery and temperature. */
	mt6357_charge_disable(chg);
	chg->charge_ctl = true;
	return devm_add_action_or_reset(dev, mt6357_charge_release, chg);
}

static void mt6357_charger_work(struct work_struct *work)
{
	struct mt6357_charger *chg = container_of(to_delayed_work(work),
						  struct mt6357_charger, work);
	bool vbat_ok, online, changed;
	int uv;

	mutex_lock(&chg->lock);
	if (chg->stopping) {
		mutex_unlock(&chg->lock);
		return;
	}
	online = mt6357_charger_online(chg);
	changed = online != chg->online;
	if (online && !chg->online)
		chg->online_since = jiffies;
	chg->online = online;
	if (mt6357_read_fg_current(chg, &chg->current_ua))
		chg->current_ua = 0;
	if (mt6357_read_temp(chg, &chg->temp_decic))
		chg->temp_decic = INT_MIN;
	vbat_ok = iio_read_channel_processed(chg->vbat, &uv) >= 0;
	if (vbat_ok) {
		chg->vbat_uv = uv * 1000;
		mt6357_gauge_update(chg);
	}
	if (chg->charge_ctl)
		mt6357_charge_update(chg, vbat_ok);
	mutex_unlock(&chg->lock);

	if (changed) {
		power_supply_changed(chg->psy);
		power_supply_changed(chg->bat);
	}
	schedule_delayed_work(&chg->work, POLL_INTERVAL);
}

static int mt6357_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = mt6357_charger_online(chg);
	return 0;
}

static int mt6357_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);
	int full = chg->info->charge_full_design_uah;
	int ret = 0;

	mutex_lock(&chg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mt6357_battery_status(chg);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = chg->info->technology;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->vbat_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = chg->current_ua;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (chg->temp_decic == INT_MIN)
			ret = -ENODATA;
		else
			val->intval = chg->temp_decic;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!chg->gauge_ready)
			ret = -ENODATA;
		else
			val->intval = DIV_ROUND_CLOSEST_ULL((u64)chg->charge_uah * 100, full);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (!chg->gauge_ready)
			ret = -ENODATA;
		else
			val->intval = chg->charge_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = full;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chg->info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chg->info->voltage_min_design_uv;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&chg->lock);
	return ret;
}

static const struct power_supply_desc mt6357_charger_desc = {
	.name = "mt6357-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = mt6357_charger_props,
	.num_properties = ARRAY_SIZE(mt6357_charger_props),
	.get_property = mt6357_charger_get_property,
};

static const struct power_supply_desc mt6357_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6357_battery_props,
	.num_properties = ARRAY_SIZE(mt6357_battery_props),
	.get_property = mt6357_battery_get_property,
};

static irqreturn_t mt6357_charger_irq(int irq, void *data)
{
	struct mt6357_charger *chg = data;

	/* The poll reads the new state (under the lock) and reports it. */
	mod_delayed_work(system_wq, &chg->work, 0);
	return IRQ_HANDLED;
}

static void mt6357_charger_cancel(void *data)
{
	struct mt6357_charger *chg = data;

	cancel_delayed_work_sync(&chg->work);
}

static int mt6357_charger_probe(struct platform_device *pdev)
{
	struct mt6397_chip *pmic = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct device *dev = &pdev->dev;
	struct mt6357_charger *chg;
	int irq, ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->dev = dev;
	chg->regmap = pmic->regmap;
	chg->temp_decic = INT_MIN;
	ret = devm_mutex_init(dev, &chg->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&chg->work, mt6357_charger_work);

	chg->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(chg->vbat))
		return dev_err_probe(dev, PTR_ERR(chg->vbat), "no vbat channel\n");
	chg->temp = devm_iio_channel_get(dev, "temp");
	if (IS_ERR(chg->temp))
		return dev_err_probe(dev, PTR_ERR(chg->temp), "no temp channel\n");

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(dev);
	chg->psy = devm_power_supply_register(dev, &mt6357_charger_desc, &cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, PTR_ERR(chg->psy),
				     "Failed to register power supply\n");

	ret = power_supply_get_battery_info(chg->psy, &chg->info);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	if (chg->info->charge_full_design_uah <= 0 ||
	    chg->info->constant_charge_voltage_max_uv <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "battery needs capacity and charge voltage\n");

	cfg.fwnode = NULL;
	chg->bat = devm_power_supply_register(dev, &mt6357_battery_desc, &cfg);
	if (IS_ERR(chg->bat))
		return dev_err_probe(dev, PTR_ERR(chg->bat),
				     "Failed to register battery\n");

	/* Before the cancel action, so on removal the charger is released after the poll stops. */
	if (device_property_read_bool(dev, "rufus,charge-control")) {
		ret = mt6357_charge_init(chg);
		if (ret)
			return ret;
	}
	platform_set_drvdata(pdev, chg);

	ret = devm_add_action_or_reset(dev, mt6357_charger_cancel, chg);
	if (ret)
		return ret;

	irq = platform_get_irq_byname(pdev, "chrdet");
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(dev, irq, NULL, mt6357_charger_irq,
					IRQF_ONESHOT, "mt6357-chrdet", chg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");
	chg->irq = irq;

	chg->online = mt6357_charger_online(chg);
	chg->online_since = jiffies;
	schedule_delayed_work(&chg->work, 0);
	return 0;
}

/* Stop the poll for good (shutdown) or until resume, and switch charging off. */
static void mt6357_charge_stop(struct mt6357_charger *chg)
{
	mutex_lock(&chg->lock);
	chg->stopping = true;
	mutex_unlock(&chg->lock);
	cancel_delayed_work_sync(&chg->work);
	mt6357_charge_disable(chg);
	chg->charging = false;
}

/* Nothing supervises charging while suspended, so it is off then. */
static int mt6357_charger_suspend(struct device *dev)
{
	struct mt6357_charger *chg = dev_get_drvdata(dev);

	if (chg->charge_ctl)
		mt6357_charge_stop(chg);
	return 0;
}

static int mt6357_charger_resume(struct device *dev)
{
	struct mt6357_charger *chg = dev_get_drvdata(dev);

	if (chg->charge_ctl) {
		mutex_lock(&chg->lock);
		chg->stopping = false;
		mutex_unlock(&chg->lock);
		mod_delayed_work(system_wq, &chg->work, 0);
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6357_charger_pm, mt6357_charger_suspend, mt6357_charger_resume);

static const struct of_device_id mt6357_charger_of_match[] = {
	{ .compatible = "mediatek,mt6357-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6357_charger_of_match);

/* Reboot and power off: stop charging; the bootloader and Android set it up again. */
static void mt6357_charger_shutdown(struct platform_device *pdev)
{
	struct mt6357_charger *chg = platform_get_drvdata(pdev);

	if (!chg || !chg->charge_ctl)
		return;
	/* A plug event now must not restart the poll after the charger is released. */
	disable_irq(chg->irq);
	mt6357_charge_stop(chg);
	mt6357_charge_release(chg);
}

static struct platform_driver mt6357_charger_driver = {
	.probe = mt6357_charger_probe,
	.shutdown = mt6357_charger_shutdown,
	.driver = {
		.name = "mt6357-charger",
		.pm = pm_sleep_ptr(&mt6357_charger_pm),
		.of_match_table = mt6357_charger_of_match,
	},
};
module_platform_driver(mt6357_charger_driver);

MODULE_DESCRIPTION("MediaTek MT6357 charger detection and fuel gauge");
MODULE_LICENSE("GPL");
