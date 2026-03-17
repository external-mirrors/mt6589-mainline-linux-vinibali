// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTD1295 Hardware Random Number Generator Driver
 * Based on reverse engineering and vendor BSP hints
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>

#define RTD1295_RNG_BASE	0x98012000
#define RTD1295_RNG_CTRL	0x00
#define RTD1295_RNG_DATA	0x04
#define RTD1295_RNG_ENABLE	BIT(0)

struct rtd1295_rng {
	void __iomem *base;
	struct hwrng rng;
};

static int rtd1295_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct rtd1295_rng *priv = container_of(rng, struct rtd1295_rng, rng);
	u32 *buf = data;
	size_t read = 0;
	int timeout;

	while (read < max && read < 4) {
		/* Enable RNG */
		writel(RTD1295_RNG_ENABLE, priv->base + RTD1295_RNG_CTRL);
		
		/* Wait for data ready (simple timeout) */
		timeout = 100;
		while (timeout-- > 0) {
			udelay(10);
			if (readl(priv->base + RTD1295_RNG_CTRL) & BIT(1))
				break;
		}
		
		if (timeout <= 0)
			break;
			
		/* Read random data */
		*buf++ = readl(priv->base + RTD1295_RNG_DATA);
		read += 4;
	}

	return read;
}

static int rtd1295_rng_probe(struct platform_device *pdev)
{
	struct rtd1295_rng *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_ioremap(&pdev->dev, RTD1295_RNG_BASE, 0x100);
	if (!priv->base)
		return -ENOMEM;

	priv->rng.name = "rtd1295-rng";
	priv->rng.read = rtd1295_rng_read;
	priv->rng.quality = 1000;

	ret = devm_hwrng_register(&pdev->dev, &priv->rng);
	if (ret) {
		dev_err(&pdev->dev, "failed to register hwrng\n");
		return ret;
	}

	dev_info(&pdev->dev, "RTD1295 hardware RNG registered\n");
	return 0;
}

static const struct of_device_id rtd1295_rng_of_match[] = {
	{ .compatible = "realtek,rtd1295-rng", },
	{}
};
MODULE_DEVICE_TABLE(of, rtd1295_rng_of_match);

static struct platform_driver rtd1295_rng_driver = {
	.probe = rtd1295_rng_probe,
	.driver = {
		.name = "rtd1295-rng",
		.of_match_table = rtd1295_rng_of_match,
	},
};

module_platform_driver(rtd1295_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RTD1295 Community");
MODULE_DESCRIPTION("Realtek RTD1295 Hardware RNG Driver");
