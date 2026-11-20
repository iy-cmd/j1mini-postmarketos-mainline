// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal AP_CLK UART clock mux driver for Spreadtrum SC7730/SC8830.
 *
 * Register AP_CLK_UART0_CFG (0x71200064):
 *   bits [1:0]  = parent mux: 0=ext_26m, 1=clk_48m, 2=clk_51m2, 3=clk_96m
 *   bits [10:8] = divider (0 = /1)
 *
 * Needed so sprd_serial can switch UART0 from 26 MHz to 48 MHz and hit
 * 3,000,000 bps (quot=16, 0.0% error) for SC2331 (Marlin 1) Bluetooth HCI.
 */

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define AP_CLK_UART_DIV_MASK	GENMASK(10, 8)
#define AP_CLK_UART_MAX_PARENTS	4

static DEFINE_SPINLOCK(sc7730_uart_clk_lock);

static int sc7730_uart_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *parents[AP_CLK_UART_MAX_PARENTS];
	struct resource *res;
	void __iomem *reg;
	struct clk_hw *hw;
	int num_parents;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	/* Shared AP_CLK page, so avoid request_mem_region */
	reg = devm_ioremap(dev, res->start, resource_size(res));
	if (!reg)
		return -ENOMEM;

	num_parents = of_clk_parent_fill(dev->of_node, parents,
					 AP_CLK_UART_MAX_PARENTS);
	if (num_parents <= 0)
		return dev_err_probe(dev, -EINVAL, "no parent clocks\n");

	/* Ensure divider = /1 */
	writel(readl(reg) & ~AP_CLK_UART_DIV_MASK, reg);

	hw = devm_clk_hw_register_mux(dev, dev_name(dev), parents, num_parents,
				      CLK_SET_RATE_PARENT, reg, 0, 2, 0,
				      &sc7730_uart_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, hw);
}

static const struct of_device_id sc7730_uart_clk_of_match[] = {
	{ .compatible = "sprd,sc7730-uart-clk" },
	{ }
};

static struct platform_driver sc7730_uart_clk_driver = {
	.probe = sc7730_uart_clk_probe,
	.driver = {
		.name = "sc7730-uart-clk",
		.of_match_table = sc7730_uart_clk_of_match,
	},
};
builtin_platform_driver(sc7730_uart_clk_driver);
