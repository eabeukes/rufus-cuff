// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6739 RDMA0 memory-mode scanout for a firmware-initialised DSI video panel.
 *
 * LK leaves OVL0 -> RDMA0 -> DSI0 running in direct-link mode, but OVL0 keeps fetching
 * LK's own images whatever the CPU writes to the layer buffers (cause unknown). RDMA0 in
 * memory mode reads the buffer directly, so point it at the simple-framebuffer and switch
 * OVL0's layers off. Runs at subsys_initcall, before simpledrm and fbcon.
 *
 * DT: compatible = "rufus,mt6739-rdma-scanout";
 *     reg = <RDMA0>, <OVL0>;
 *     memory-region-fb = <&simple_framebuffer_node>;  (its reg and stride are used)
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#define OVL_SRC_CON		0x02c

#define RDMA_GLOBAL_CON		0x010
#define RDMA_MODE_MEMORY	BIT(1)
#define RDMA_MEM_CON		0x024
#define RDMA_MEM_FMT_XRGB8888	0x20	/* input format 2, no byte swap */
#define RDMA_MEM_SRC_PITCH	0x02c
#define RDMA_MEM_START_ADDR	0xf00

static int rdma_scanout_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *rdma, *ovl;
	struct device_node *fb;
	struct resource res;
	u32 stride;
	int ret;

	rdma = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rdma))
		return PTR_ERR(rdma);
	ovl = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ovl))
		return PTR_ERR(ovl);

	fb = of_parse_phandle(dev->of_node, "memory-region-fb", 0);
	if (!fb)
		return dev_err_probe(dev, -EINVAL, "no memory-region-fb\n");
	ret = of_address_to_resource(fb, 0, &res);
	if (!ret)
		ret = of_property_read_u32(fb, "stride", &stride);
	of_node_put(fb);
	if (ret)
		return dev_err_probe(dev, ret, "bad framebuffer node\n");
	if (upper_32_bits(res.start))
		return dev_err_probe(dev, -EINVAL, "framebuffer above 4 GiB\n");

	writel(0, ovl + OVL_SRC_CON);
	writel(lower_32_bits(res.start), rdma + RDMA_MEM_START_ADDR);
	writel(stride, rdma + RDMA_MEM_SRC_PITCH);
	writel(RDMA_MEM_FMT_XRGB8888, rdma + RDMA_MEM_CON);
	writel(readl(rdma + RDMA_GLOBAL_CON) | RDMA_MODE_MEMORY, rdma + RDMA_GLOBAL_CON);

	dev_info(dev, "RDMA0 scanning %pR, stride %u\n", &res, stride);
	return 0;
}

static const struct of_device_id rdma_scanout_of_match[] = {
	{ .compatible = "rufus,mt6739-rdma-scanout" },
	{ }
};

static struct platform_driver rdma_scanout_driver = {
	.probe = rdma_scanout_probe,
	.driver = {
		.name = "mt6739-rdma-scanout",
		.of_match_table = rdma_scanout_of_match,
	},
};

static int __init rdma_scanout_init(void)
{
	return platform_driver_register(&rdma_scanout_driver);
}
subsys_initcall(rdma_scanout_init);

MODULE_DESCRIPTION("MT6739 RDMA0 memory-mode scanout");
MODULE_LICENSE("GPL");
