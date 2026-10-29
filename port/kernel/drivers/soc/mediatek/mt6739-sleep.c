// SPDX-License-Identifier: GPL-2.0
/*
 * MT6739 suspend to RAM through the stock ATF and its SPM firmware.
 *
 * The ATF answers PSCI_FEATURES for SYSTEM_SUSPEND, but a SYSTEM_SUSPEND never comes back
 * (measured: syscore suspend ran, syscore resume never did, the watchdog reset the watch).
 * The vendor kernel does it differently: it hands the SPM its suspend arguments with a SiP
 * call, then either enters PSCI CPU_SUSPEND with the "suspend" power state from its device
 * tree, 0x01010005 (the CPU powered down), or, with DIS_CPU_PDN in the flags, asks the ATF for
 * a "legacy sleep" that keeps the CPU powered. The first resumes here, but the boot CPU then
 * hangs in PSCI CPU_ON for CPU1 (measured), so the legacy sleep is the default. It doesn't
 * sleep yet either: it returns at once with no wake reason in the SPM registers.
 *
 * In the legacy sleep the CPU waits in WFI for the SPM's interrupt, so that one stays
 * unmasked through suspend (as the vendor's mt_irq_unmask_for_sleep_ex(SPM_IRQ0_ID)), and its
 * status is cleared through the ATF.
 */
#include <linux/arm-smccc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/suspend.h>

#define MTK_SIP_SPM_SUSPEND_ARGS	0xc200021a
#define MTK_SIP_SPM_IRQ0_HANDLER	0xc200021c
#define MTK_SIP_SPM_LEGACY_SLEEP	0xc200021f
#define MTK_SIP_SPM_ARGS		0xc2000228
#define SPM_ARGS_SUSPEND		2
#define SPM_ARGS_SUSPEND_FINISH		3
#define PCM_FLAG_DIS_CPU_PDN		BIT(0)
#define MT6739_PSCI_SUSPEND		0x01010005
#define SPM_TIMER_HZ			32768
#define SPM_IRQ_STA			0x158

static void __iomem *spm_base;

/*
 * The vendor's MT6739 suspend flags: DIS_VCORE_DVS | DIS_VCORE_DFS | DIS_ATF_ABORT |
 * KEEP_CSYSPWRUPACK_HIGH.
 */
static uint pcm_flags = 0x198;
module_param(pcm_flags, uint, 0644);

/* Power the CPU cluster down (PSCI CPU_SUSPEND) instead of the legacy sleep. */
static bool cpu_pdn;
module_param(cpu_pdn, bool, 0644);

/* Wake from the SPM's own timer after this many seconds; 0 leaves the ATF's maximum. */
static uint timer_s;
module_param(timer_s, uint, 0644);

static int mt6739_sleep_enter(suspend_state_t state)
{
	u32 flags = cpu_pdn ? pcm_flags & ~PCM_FLAG_DIS_CPU_PDN : pcm_flags | PCM_FLAG_DIS_CPU_PDN;
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_SPM_SUSPEND_ARGS, flags, 0, timer_s * SPM_TIMER_HZ, 0, 0, 0, 0, &res);
	if (cpu_pdn)
		return psci_cpu_suspend_enter(MT6739_PSCI_SUSPEND);

	arm_smccc_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND, 0, 0, 0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_SPM_LEGACY_SLEEP, 0, 0, 0, 0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND_FINISH, 0, 0, 0, 0, 0, 0, &res);
	return 0;
}

static const struct platform_suspend_ops mt6739_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.enter = mt6739_sleep_enter,
};

static irqreturn_t mt6739_sleep_irq(int irq, void *data)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_SPM_IRQ0_HANDLER, readl(spm_base + SPM_IRQ_STA), 0, 0, 0, 0, 0, 0,
		      &res);
	return IRQ_HANDLED;
}

static int mt6739_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int irq, ret;

	/* scpsys owns these registers; only SPM_IRQ_STA is read here. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	spm_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!spm_base)
		return -ENOMEM;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mt6739_sleep_irq, IRQF_NO_SUSPEND, "mt6739-spm", NULL);
	if (ret)
		return ret;

	/* Probed after psci_dt_init(), whose SYSTEM_SUSPEND ops this replaces. */
	suspend_set_ops(&mt6739_sleep_ops);
	return 0;
}

static const struct of_device_id mt6739_sleep_of_match[] = {
	{ .compatible = "mediatek,mt6739-sleep" },
	{ }
};

static struct platform_driver mt6739_sleep_driver = {
	.probe = mt6739_sleep_probe,
	.driver = {
		.name = "mt6739-sleep",
		.suppress_bind_attrs = true,
		.of_match_table = mt6739_sleep_of_match,
	},
};
builtin_platform_driver(mt6739_sleep_driver);
