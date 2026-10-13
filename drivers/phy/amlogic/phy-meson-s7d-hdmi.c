// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* S7D HDMI analogue PHY. Sequencing derived from Amlogic hdmitx_hw_s7d.c. */
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define PHY_CTRL0	0x200
#define PHY_CTRL3	0x20c
#define PHY_CTRL5	0x214
#define PHY_LANES_ON	GENMASK(29, 28)
#define PHY_RESET_N	(GENMASK(11, 10) | BIT(4))
#define S7D_HDMI_SERVICE	0x820000ab
#define S7D_GET_RTERM	10

struct s7d_hdmi_phy {
	struct regmap *map;
	struct clk *pixel;
	struct reset_control *reset;
	u32 rterm;
};

static int s7d_phy_stop(struct s7d_hdmi_phy *priv)
{
	int ret;

	ret = regmap_write(priv->map, PHY_CTRL0, 0);
	if (ret)
		return ret;
	ret = regmap_write(priv->map, PHY_CTRL5, 0);
	if (ret)
		return ret;
	ret = regmap_write(priv->map, PHY_CTRL3, 0x304efc1b);
	if (ret)
		return ret;
	return regmap_write(priv->map, PHY_CTRL3, 0xc1b);
}

static int s7d_phy_power_on(struct phy *phy)
{
	struct s7d_hdmi_phy *priv = phy_get_drvdata(phy);
	unsigned long rate;
	u32 ctrl0, ctrl3;
	bool active;
	int ret;

	/* Snapshot before acquiring resources which may change hardware state. */
	ret = reset_control_status(priv->reset);
	if (ret < 0)
		return ret;
	active = !ret;
	ret = regmap_read(priv->map, PHY_CTRL0, &ctrl0);
	if (ret)
		return ret;
	ret = regmap_read(priv->map, PHY_CTRL3, &ctrl3);
	if (ret)
		return ret;
	active &= ctrl0 && (ctrl3 & (PHY_LANES_ON | PHY_RESET_N)) ==
			  (PHY_LANES_ON | PHY_RESET_N);

	/* A live transmitter's analogue rate must not change behind its back. */
	ret = clk_rate_exclusive_get(priv->pixel);
	if (ret)
		return ret;
	rate = clk_get_rate(priv->pixel);
	if (rate < 25175000 || rate > 594000000) {
		ret = -EINVAL;
		goto release_rate;
	}
	ret = clk_prepare_enable(priv->pixel);
	if (ret)
		goto release_rate;
	/* Preserve active analogue state; this is not full display validation. */
	if (active)
		return 0;
	ret = reset_control_deassert(priv->reset);
	if (ret)
		goto disable_clock;

	ctrl0 = rate > 300000000 ? 0x3cafb :
		rate > 150000000 ? 0x380dd : 0x2038088;
	ret = regmap_write(priv->map, PHY_CTRL0, ctrl0 | priv->rterm);
	if (ret)
		goto stop;
	ret = regmap_write(priv->map, PHY_CTRL5, 0x2555);
	if (ret)
		goto stop;
	ret = regmap_write(priv->map, PHY_CTRL3, 0x4ef00b);
	if (ret)
		goto stop;
	usleep_range(100, 110);
	ret = regmap_update_bits(priv->map, PHY_CTRL3, PHY_RESET_N, PHY_RESET_N);
	if (ret)
		goto stop;
	usleep_range(1000, 1010);
	ret = regmap_update_bits(priv->map, PHY_CTRL3, PHY_LANES_ON, PHY_LANES_ON);
	if (!ret)
		return 0;
stop:
	s7d_phy_stop(priv);
disable_clock:
	clk_disable_unprepare(priv->pixel);
release_rate:
	clk_rate_exclusive_put(priv->pixel);
	return ret;
}

static int s7d_phy_power_off(struct phy *phy)
{
	struct s7d_hdmi_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = s7d_phy_stop(priv);
	if (ret)
		return ret;
	/*
	 * CTRL3=0xc1b parks the lanes but keeps the PHY-to-digital TMDS path.
	 * The TX core is still used between power_off and the next power_on
	 * (including modeset configuration). Holding HDMITXPHY in reset here
	 * defeats that retained clock path. Vendor S7D hdmi_phy_suspend() also
	 * leaves this reset deasserted; power_off is not a controller reset.
	 */
	clk_disable_unprepare(priv->pixel);
	clk_rate_exclusive_put(priv->pixel);
	return 0;
}

static const struct phy_ops s7d_phy_ops = {
	.power_on = s7d_phy_power_on,
	.power_off = s7d_phy_power_off,
	.owner = THIS_MODULE,
};

static int s7d_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct arm_smccc_res res;
	struct s7d_hdmi_phy *priv;
	struct phy *phy;
	u8 rterm;
	int ret;

	if (!dev->parent ||
	    !of_device_is_compatible(dev->parent->of_node, "amlogic,s7d-clkc"))
		return -EINVAL;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	/* The parent owns the MMIO resource; do not map it a second time. */
	priv->map = dev_get_regmap(dev->parent, "analog");
	if (!priv->map)
		return dev_err_probe(dev, -ENODEV, "missing parent analogue regmap\n");
	priv->pixel = devm_clk_get(dev, "pixel");
	if (IS_ERR(priv->pixel))
		return dev_err_probe(dev, PTR_ERR(priv->pixel), "missing pixel clock\n");
	priv->reset = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset), "missing PHY reset\n");

	/* Firmware service reads trim fuses; it does not enable HDCP. */
	arm_smccc_smc(S7D_HDMI_SERVICE, S7D_GET_RTERM, 0, 0, 0, 0, 0, 0, &res);
	rterm = res.a0 & 0xff;
	priv->rterm = (u32)(rterm <= 0xf ? rterm : 8) << 28;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	phy = devm_phy_create(dev, NULL, &s7d_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, priv);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id s7d_phy_match[] = {
	{ .compatible = "amlogic,s7d-hdmi-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_phy_match);

static struct platform_driver s7d_phy_driver = {
	.probe = s7d_phy_probe,
	.driver = {
		.name = "s7d-hdmi-phy",
		.of_match_table = s7d_phy_match,
	},
};
module_platform_driver(s7d_phy_driver);

MODULE_DESCRIPTION("Amlogic S7D HDMI analogue PHY");
MODULE_LICENSE("GPL");
