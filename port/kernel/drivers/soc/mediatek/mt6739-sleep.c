// SPDX-License-Identifier: GPL-2.0
/*
 * MT6739 suspend to RAM through the stock ATF and its SPM firmware.
 *
 * The ATF answers PSCI_FEATURES for SYSTEM_SUSPEND, but a SYSTEM_SUSPEND never comes back
 * (measured: syscore suspend ran, syscore resume never did, the watchdog reset the watch).
 * The vendor kernel does it differently: it hands the SPM its suspend arguments with a SiP
 * call, then enters PSCI CPU_SUSPEND with the "suspend" power state from its device tree,
 * 0x01010005: the SPM powers the CPU cluster down and back up on a wake event (measured: wakes
 * on the SPM timer, all CPUs come back). With DIS_CPU_PDN in the flags it asks the ATF for a
 * "legacy sleep" that keeps the CPU powered instead; that one sleeps but never wakes here
 * (measured), so it is only left as an option.
 *
 * Like the vendor, this masks every interrupt at the GIC except the SPM's first
 * (mt_irq_mask_all, mt_irq_unmask_for_sleep_ex(SPM_IRQ0_ID)): the ATF just executes WFI after
 * kicking the SPM, so any pending interrupt ends the sleep. The SPM interrupt's status is
 * cleared through the ATF.
 */
#include <linux/arm-smccc.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/suspend.h>
#include <asm/arch_timer.h>

#define MTK_SIP_POWER_DOWN_CORE		0xc2000218
#define MTK_SIP_SPM_SUSPEND_ARGS	0xc200021a
#define MTK_SIP_SPM_IRQ0_HANDLER	0xc200021c
#define MTK_SIP_SPM_LEGACY_SLEEP	0xc200021f
#define MTK_SIP_SPM_DUMMY_READ		0xc2000225
#define MTK_SIP_SPM_ARGS		0xc2000228
#define SPM_ARGS_SUSPEND		2
#define SPM_ARGS_SUSPEND_FINISH		3
#define PCM_FLAG_DIS_CPU_PDN		BIT(0)
#define MT6739_PSCI_SUSPEND		0x01010005
#define SPM_TIMER_HZ			32768
#define SPM_IRQ_STA			0x158
#define GIC_MAX_ENABLE_REGS		32

static void __iomem *spm_base, *gicd_base, *gicr_sgi_base;
static int spm_hwirq;

/*
 * The vendor's MT6739 suspend flags: DIS_VCORE_DVS | DIS_VCORE_DFS | DIS_ATF_ABORT |
 * KEEP_CSYSPWRUPACK_HIGH.
 */
static uint pcm_flags = 0x198;
module_param(pcm_flags, uint, 0644);

/* Power the CPU cluster down (PSCI CPU_SUSPEND); off asks for the legacy sleep. */
static bool cpu_pdn = true;
module_param(cpu_pdn, bool, 0644);

/* Wake from the SPM's own timer after this many seconds; 0 leaves the ATF's maximum. */
static uint timer_s;
module_param(timer_s, uint, 0644);

/* Leave only the SPM interrupt enabled at the GIC; @saved gets the enables to restore. */
static int mt6739_sleep_mask_gic(u32 *saved)
{
	int i, n = min_t(int, (readl(gicd_base + GICD_TYPER) & 0x1f) + 1, GIC_MAX_ENABLE_REGS);

	saved[0] = readl(gicr_sgi_base + GICR_ISENABLER0);
	writel(~0, gicr_sgi_base + GICR_ICENABLER0);
	for (i = 1; i < n; i++) {
		saved[i] = readl(gicd_base + GICD_ISENABLER + 4 * i);
		writel(~0, gicd_base + GICD_ICENABLER + 4 * i);
	}
	writel(BIT(spm_hwirq % 32), gicd_base + GICD_ISENABLER + 4 * (spm_hwirq / 32));
	return n;
}

static void mt6739_sleep_restore_gic(const u32 *saved, int n)
{
	int i;

	for (i = 1; i < n; i++)
		writel(saved[i], gicd_base + GICD_ISENABLER + 4 * i);
	writel(saved[0], gicr_sgi_base + GICR_ISENABLER0);
}

static int mt6739_sleep_enter(suspend_state_t state)
{
	u32 flags = cpu_pdn ? pcm_flags & ~PCM_FLAG_DIS_CPU_PDN : pcm_flags | PCM_FLAG_DIS_CPU_PDN;
	u32 saved[GIC_MAX_ENABLE_REGS];
	struct arm_smccc_res res;
	int n, ret = 0;
	u64 t0, t1;

	arm_smccc_smc(MTK_SIP_SPM_SUSPEND_ARGS, flags, 0, timer_s * SPM_TIMER_HZ, 0, 0, 0, 0, &res);
	n = mt6739_sleep_mask_gic(saved);
	t0 = __arch_counter_get_cntvct();
	if (cpu_pdn) {
		ret = psci_cpu_suspend_enter(MT6739_PSCI_SUSPEND);
	} else {
		arm_smccc_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND, 0, 0, 0, 0, 0, 0, &res);
		arm_smccc_smc(MTK_SIP_SPM_LEGACY_SLEEP, 0, 0, 0, 0, 0, 0, 0, &res);
	}
	t1 = __arch_counter_get_cntvct();
	if (!cpu_pdn)
		arm_smccc_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND_FINISH, 0, 0, 0, 0, 0, 0, &res);
	mt6739_sleep_restore_gic(saved, n);

	/* The ATF logs the wake reason (spm_wake_reason) in its log buffer. */
	pr_info("mt6739-sleep: slept %llu ms, rc %d\n",
		div_u64(t1 - t0, arch_timer_get_rate() / 1000), ret);
	return ret;
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

/*
 * PSCI CPU_OFF in this ATF only parks the core in WFI; the vendor kernel then has a surviving
 * CPU power it off (MTK_SIP_POWER_DOWN_CORE from its CPU_DEAD notifier). A parked core is still
 * powered, so it comes out of reset with the cluster after a suspend, and the ATF panics on it
 * ("Unexpected affinity info state", measured).
 */
static int mt6739_sleep_cpu_dead(unsigned int cpu)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CORE, cpu, 0, 0, 0, 0, 0, 0, &res);
	return 0;
}

/*
 * Waking from DDR PHY power-down, the SPM firmware reads a word from each DRAM rank, at the
 * buffers LK reserves for that (as the vendor's spm_set_dummy_read_addr()). The ATF puts them
 * in SPM_SW_RSV registers; unset, they're 0.
 */
static int mt6739_sleep_set_dummy_read(void)
{
	struct arm_smccc_res res;
	phys_addr_t rank[2];
	int i;

	for (i = 0; i < 2; i++) {
		char compat[] = "reserve-memory-dram_rN_dummy_read";
		struct reserved_mem *rmem;
		struct device_node *np;

		compat[sizeof("reserve-memory-dram_r") - 1] = '0' + i;
		np = of_find_compatible_node(NULL, NULL, compat);
		rmem = np ? of_reserved_mem_lookup(np) : NULL;
		of_node_put(np);
		if (rmem)
			rank[i] = rmem->base;
		else if (i)
			rank[i] = rank[0];	/* one rank */
		else
			return -ENODEV;
	}
	arm_smccc_smc(MTK_SIP_SPM_DUMMY_READ, rank[0], rank[1], 0, 0, 0, 0, 0, &res);
	return 0;
}

static int mt6739_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *gic;
	struct irq_data *d;
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
	/* The GIC's interrupt ID, under the sysirq polarity inverter. */
	d = irq_get_irq_data(irq);
	while (d->parent_data)
		d = d->parent_data;
	spm_hwirq = irqd_to_hwirq(d);

	/* The GIC driver owns these too; they're only touched with interrupts off in enter. */
	gic = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!gic)
		return -ENODEV;
	gicd_base = of_iomap(gic, 0);
	gicr_sgi_base = of_iomap(gic, 1);
	of_node_put(gic);
	if (!gicd_base || !gicr_sgi_base)
		return -ENOMEM;
	/* The boot CPU's (affinity 0) redistributor comes first; its SGI/PPI frame is the second 64K. */
	if (readq(gicr_sgi_base + GICR_TYPER) >> 32)
		return -ENODEV;
	gicr_sgi_base += SZ_64K;

	ret = mt6739_sleep_set_dummy_read();
	if (ret)
		return ret;

	/* Teardown of a BP state runs on a surviving CPU once the CPU is dead. */
	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "soc/mt6739-sleep:dead", NULL,
					mt6739_sleep_cpu_dead);
	if (ret < 0)
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
