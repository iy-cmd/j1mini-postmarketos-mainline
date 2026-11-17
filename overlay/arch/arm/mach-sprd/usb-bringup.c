// SPDX-License-Identifier: GPL-2.0
/*
 * Power up the SC7730 / SC8830 USB PHY so the generic dwc2 driver can run the
 * controller in device mode. Bring-up hack until there are proper clock,
 * PHY and SC2723 regulator drivers.
 *
 * Sequence and register map from the vendor 3.10 kernel
 * (drivers/usb/gadget/dwc_otg/usb_hw.c: udc_enable() + usb_phy_init()):
 *
 *   1. SC2723 LDO_USB on: ANA_GLB LDO_PD_CTRL bit 8, behind the ADI bus
 *   2. USB PHY out of power-down: AON_APB PWR_CTRL bit 0
 *   3. USB AHB clock on: AP_AHB EB bit 4
 *
 * For J1 Mini (chip_x20 / SC8830 / SC7727S):
 *   4. AP_AHB_RST bits 5, 6 pulsed
 *   5. Set GUSBCFG (0x2020000c) bit 3 for 16-bit UTMI+ PHY width
 *   6. Reset SPRD PHY: utmi_PORN (PIN_BASE 0x402A0000 bit 0) & utmi_rst (AHB_RST bit 7)
 *   7. Write AP_APB REG_AP_APB_USB_CTRL0 (0x71300010) = 0xd1a00020
 *   8. Write AP_APB REG_AP_APB_USB_CTRL1 (0x71300014) = 0x00001730
 *
 * For gtelwifi (chip_x30g / SC7730):
 *   4. pulse USB, UTMI and PHY soft resets: AP_AHB RST bits 5..7
 *   5. AP_APB USB_PHY_CTRL bit 9 and GTELWIFI_PHY_TUNE (0x44076e33)
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/sprintf.h>

#include "adi.h"

#define ANA_LDO_PD_CTRL		SC2723_GLB(0x14)
#define ANA_LDO_SIM2_PD		BIT(3)
#define ANA_LDO_USB_PD		BIT(8)
#define ANA_LDO_SDCORE_PD	BIT(10)
#define ANA_LDO_V_CTRL5		SC2723_GLB(0x2c)	/* bits 15:8 LDO_SIM2_V */
#define ANA_LDO_V_CTRL6		SC2723_GLB(0x30)	/* bits 15:8 LDO_SDCORE_V */
#define ANA_LDO_V_CTRL9		SC2723_GLB(0x3c)	/* bits 7:0 LDO_USB_V */

#define AON_APB_PHYS		0x402e0000
#define AON_APB_EB0		0x00
#define AON_APB_PWR_CTRL	0x24
#define AON_USB_PHY_PD		BIT(0)

#define AP_AHB_PHYS		0x20d00000
#define AP_AHB_EB		0x00
#define AP_AHB_USB_EB		BIT(4)
#define AP_AHB_RST		0x04
#define AP_AHB_USB_RSTS		(BIT(5) | BIT(6) | BIT(7))

#define AP_APB_PHYS		0x71300000
#define AP_APB_USB_PHY_TUNE	0x3000
#define AP_APB_USB_PHY_CTRL	0x3008
#define AP_APB_PHY_CTRL_BIT9	BIT(9)

/* J1 Mini (chip_x20) registers */
#define SPRD_PIN_PHYS		0x402a0000
#define REG_PIN_CTRL0		0x00
#define PIN_UTMI_PORN		BIT(0)

#define REG_AP_APB_USB_CTRL0	0x10
#define REG_AP_APB_USB_CTRL1	0x14

#define USB_PHYS		0x20200000
#define DWC2_GUSBCFG		0x0c
#define DWC2_GRXFSIZ		0x24
#define DWC2_GNPTXFSIZ		0x28
#define DWC2_GSNPSID		0x40
#define DWC2_GHWCFG(n)		(0x44 + 4 * ((n) - 1))
#define DWC2_DPTXFSIZN(n)	(0x104 + 4 * ((n) - 1))

/* "tune_value" of the vendor usb node */
#define GTELWIFI_PHY_TUNE	0x44076e33
#define J1MINI_PHY_TUNE_CTRL0	0xd1a00020
#define J1MINI_PHY_TUNE_CTRL1	0x00001730

static void __init rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

static int __init sprd_usb_bringup(void)
{
	void __iomem *aon, *ahb, *apb, *usb, *pin = NULL, *apb_base = NULL;
	struct device_node *np;
	u32 pd = 0, pd_new = 0, vusb = 0;
	bool is_j1mini;
	int ret;

	if (!of_machine_is_compatible("sprd,sc7730"))
		return 0;
	np = of_find_compatible_node(NULL, NULL, "snps,dwc2");
	if (!np)
		return 0;
	ret = of_device_is_available(np);
	of_node_put(np);
	if (!ret)
		return 0;

	is_j1mini = of_machine_is_compatible("samsung,j1mini3g") ||
		    of_machine_is_compatible("samsung,j1mini");

	aon = ioremap(AON_APB_PHYS, 0x100);
	ahb = ioremap(AP_AHB_PHYS, 0x100);
	apb = ioremap(AP_APB_PHYS + AP_APB_USB_PHY_TUNE, 0x10);
	usb = ioremap(USB_PHYS, 0x1000);
	if (is_j1mini) {
		pin = ioremap(SPRD_PIN_PHYS, 0x10);
		apb_base = ioremap(AP_APB_PHYS, 0x100);
	}

	if (!aon || !ahb || !apb || !usb || (is_j1mini && (!pin || !apb_base))) {
		pr_err("sprd-usb: ioremap failed\n");
		goto out;
	}

	pr_info("sprd-usb: before: AON_EB0 %08x PWR_CTRL %08x AHB_EB %08x PHY_CTRL %08x TUNE %08x\n",
		readl(aon + AON_APB_EB0), readl(aon + AON_APB_PWR_CTRL),
		readl(ahb + AP_AHB_EB), readl(apb + 8), readl(apb));

	/* 1. LDO_USB, LDO_SIM2 (Touchscreen 3.0V), LDO_SDCORE (microSD 3.0V) */
	/* Configure 3.3V for USB: offset 0x3c (ANA_LDO_V_CTRL9), bits 7:0 = 0xd2 (210 * 10mV + 1.2V = 3.3V) */
	sprd_adi_update(ANA_LDO_V_CTRL9, 0xff, 0xd2);
	/* Configure 3.0V for SIM2 (touchscreen) and SDCORE (microSD) */
	sprd_adi_update(ANA_LDO_V_CTRL5, 0xff00, 0xb400);
	sprd_adi_update(ANA_LDO_V_CTRL6, 0xff00, 0xb400);
	ret = sprd_adi_read(ANA_LDO_PD_CTRL, &pd);
	if (!ret) {
		u32 pwr_mask = ANA_LDO_USB_PD | ANA_LDO_SIM2_PD | ANA_LDO_SDCORE_PD;
		if (pd & pwr_mask)
			ret = sprd_adi_write(ANA_LDO_PD_CTRL, pd & ~pwr_mask);
	}
	if (!ret)
		ret = sprd_adi_read(ANA_LDO_PD_CTRL, &pd_new);
	sprd_adi_read(ANA_LDO_V_CTRL9, &vusb);
	pr_info("sprd-usb: LDO_PD_CTRL %04x -> %04x (%d), LDO_V_CTRL9 %04x (target 3.3V: 0xd2)\n",
		pd, pd_new, ret, vusb);
	pr_info("sprd-usb: LDO_SIM2 (TS 3.0V) & LDO_SDCORE (microSD 3.0V) enabled\n");
	mdelay(20);

	/* 2. PHY power, AHB clock */
	rmw(aon + AON_APB_PWR_CTRL, AON_USB_PHY_PD, 0);
	rmw(ahb + AP_AHB_EB, 0, AP_AHB_USB_EB);
	mdelay(10);

	if (is_j1mini) {
		/* Pulse AHB reset bits 5 & 6 (USB + UTMI soft reset) */
		rmw(ahb + AP_AHB_RST, 0, BIT(5) | BIT(6));
		mdelay(5);
		rmw(ahb + AP_AHB_RST, BIT(5) | BIT(6), 0);

		/* Match vendor sprd_usb_phy_rst(): set GUSBCFG bit 3 (16-bit UTMI+) before PHY reset */
		rmw(usb + DWC2_GUSBCFG, 0, BIT(3));

		/* Reset PHY: utmi_PORN (PIN_BASE bit 0) & utmi_rst (AHB_RST bit 7) */
		rmw(pin + REG_PIN_CTRL0, PIN_UTMI_PORN, 0);
		rmw(ahb + AP_AHB_RST, 0, BIT(7));
		mdelay(5);
		rmw(pin + REG_PIN_CTRL0, 0, PIN_UTMI_PORN);
		rmw(ahb + AP_AHB_RST, BIT(7), 0);
		mdelay(5);

		/* Write PHY tuning registers REG_AP_APB_USB_CTRL0 and CTRL1 */
		writel(J1MINI_PHY_TUNE_CTRL0, apb_base + REG_AP_APB_USB_CTRL0);
		writel(J1MINI_PHY_TUNE_CTRL1, apb_base + REG_AP_APB_USB_CTRL1);

		/*
		 * CRITICAL: On SC8830/chip_x20, REG_AP_APB_USB_PHY_CTRL (0x71303008)
		 * BIT(9) is BIT_DMPULLUP (D- pull-up)! Setting it breaks USB HS/FS
		 * signaling with error -71. Ensure it is CLEARED.
		 */
		rmw(apb + 8, AP_APB_PHY_CTRL_BIT9, 0);

		pr_info("sprd-usb: j1mini CTRL0 %08x CTRL1 %08x GUSBCFG %08x PIN_CTRL0 %08x PHY_CTRL %08x\n",
			readl(apb_base + REG_AP_APB_USB_CTRL0),
			readl(apb_base + REG_AP_APB_USB_CTRL1),
			readl(usb + DWC2_GUSBCFG),
			readl(pin + REG_PIN_CTRL0),
			readl(apb + 8));
	} else {
		/* gtelwifi: pulse USB, UTMI and PHY soft resets */
		rmw(ahb + AP_AHB_RST, 0, AP_AHB_USB_RSTS);
		mdelay(5);
		rmw(ahb + AP_AHB_RST, AP_AHB_USB_RSTS, 0);
		mdelay(3);

		/* PHY control and tune */
		rmw(apb + 8, 0, AP_APB_PHY_CTRL_BIT9);
		writel(GTELWIFI_PHY_TUNE, apb);
	}

	pr_info("sprd-usb: after: PWR_CTRL %08x AHB_EB %08x GSNPSID %08x\n",
		readl(aon + AON_APB_PWR_CTRL), readl(ahb + AP_AHB_EB),
		readl(usb + DWC2_GSNPSID));

	/* Reset values = what the core was synthesized with (FIFO maxima) */
	pr_info("sprd-usb: GHWCFG1-4 %08x %08x %08x %08x GRXFSIZ %08x GNPTXFSIZ %08x\n",
		readl(usb + DWC2_GHWCFG(1)), readl(usb + DWC2_GHWCFG(2)),
		readl(usb + DWC2_GHWCFG(3)), readl(usb + DWC2_GHWCFG(4)),
		readl(usb + DWC2_GRXFSIZ), readl(usb + DWC2_GNPTXFSIZ));
	{
		char buf[16 * 6] = "", *p = buf;
		int n;

		for (n = 1; n <= 15; n++)
			p += scnprintf(p, buf + sizeof(buf) - p, " %x",
				       readl(usb + DWC2_DPTXFSIZN(n)) >> 16);
		pr_info("sprd-usb: DPTXFSIZ1-15 depth:%s\n", buf);
	}

	/* Force GOTGCTL B-session valid (DBNCE_FLTR_BYPASS | BVALOVAL | BVALOEN | VBVALOVAL | VBVALOEN) */
	rmw(usb + 0x000, 0, BIT(15) | BIT(7) | BIT(6) | BIT(3) | BIT(2));
	udelay(10);
	pr_info("sprd-usb: GOTGCTL set to %08x\n", readl(usb + 0x000));

	/* Assert soft disconnect so host does not enumerate before gadget driver is ready */
	writel(readl(usb + 0x804) | BIT(1), usb + 0x804);
	pr_info("sprd-usb: asserted DCTL.SFTDISCON (DCTL: %08x)\n", readl(usb + 0x804));
out:
	if (apb_base)
		iounmap(apb_base);
	if (pin)
		iounmap(pin);
	if (usb)
		iounmap(usb);
	if (apb)
		iounmap(apb);
	if (ahb)
		iounmap(ahb);
	if (aon)
		iounmap(aon);
	return 0;
}
subsys_initcall(sprd_usb_bringup);
