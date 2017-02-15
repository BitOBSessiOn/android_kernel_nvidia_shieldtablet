/*
 * palmas_wdt.c -- Palmas Watchdog Timer.
 *
 * Watchdog timer for Palmas PMIC.
 *
 * Copyright (c) 2013-2016, NVIDIA Corporation. All rights reserved.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/alarmtimer.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/palmas.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/watchdog.h>

static bool nowayout = WATCHDOG_NOWAYOUT;

struct palmas_wdt {
	struct watchdog_device wdt_dev;
	struct device *dev;
	struct palmas *palmas;
	int timeout;
	int org_timeout;
	int irq;
	int locked;
	int watchdog_timer_initial_period;
	int rtc_wakeup_period;
	int interrupt_mode;
};

static irqreturn_t palmas_wdt_irq(int irq, void *data)
{
	struct palmas_wdt *wdt = data;

	dev_info(wdt->dev, "WDT interrupt occur\n");
	return IRQ_HANDLED;
}

static int palmas_wdt_start(struct watchdog_device *wdt_dev)
{
	struct palmas_wdt *wdt = watchdog_get_drvdata(wdt_dev);
	int ret;

	if (wdt->locked) {
		dev_err(wdt->dev,
			"Watchdog timer is locked, can not control\n");
		return -EBUSY;
	}

	ret = palmas_update_bits(wdt->palmas, PALMAS_PMU_CONTROL_BASE,
			PALMAS_WATCHDOG, PALMAS_WATCHDOG_ENABLE,
			PALMAS_WATCHDOG_ENABLE);
	if (ret < 0) {
		dev_err(wdt->dev, "WATCHDOG update failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int palmas_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct palmas_wdt *wdt = watchdog_get_drvdata(wdt_dev);
	int ret;

	if (wdt->locked) {
		dev_err(wdt->dev,
			"Watchdog timer is locked, can not control\n");
		return -EBUSY;
	}

	ret = palmas_update_bits(wdt->palmas, PALMAS_PMU_CONTROL_BASE,
			PALMAS_WATCHDOG, PALMAS_WATCHDOG_ENABLE, 0);
	if (ret < 0) {
		dev_err(wdt->dev, "WATCHDOG update failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int palmas_wdt_set_timeout(struct watchdog_device *wdt_dev,
		unsigned int timeout)
{
	struct palmas_wdt *wdt = watchdog_get_drvdata(wdt_dev);
	int i;
	int ret;
	unsigned int bit = 1;

	for (i = 0; i < 7; ++i) {
		if (timeout <= bit)
			break;

		bit <<= 1;
	}

	ret = palmas_update_bits(wdt->palmas, PALMAS_PMU_CONTROL_BASE,
			PALMAS_WATCHDOG, PALMAS_WATCHDOG_TIMER_MASK, i);
	if (ret < 0) {
		dev_err(wdt->dev, "WATCHDOG update failed: %d\n", ret);
		return ret;
	}
	wdt->timeout = timeout;
	return 0;
}

static const struct watchdog_info palmas_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
	.identity = "Palmas Watchdog",
};

static const struct watchdog_ops palmas_wdt_ops = {
	.owner = THIS_MODULE,
	.start = palmas_wdt_start,
	.stop = palmas_wdt_stop,
	.set_timeout = palmas_wdt_set_timeout,
};

static int palmas_wdt_restart(struct watchdog_device *wdt_dev,
		unsigned int timeout)
{
	int ret;

	if (!timeout)
		return 0;

	ret = palmas_wdt_set_timeout(wdt_dev, timeout);
	if (!ret)
		ret = palmas_wdt_start(wdt_dev);

	return ret;
}

static ssize_t show_wdt_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct palmas_wdt *wdt = dev_get_drvdata(dev);

	return sprintf(buf, "%s",
			(wdt->timeout) ? "enable\n" : "disable\n");
}

static ssize_t set_wdt_state(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct palmas_wdt *wdt = dev_get_drvdata(dev);
	int enable;
	char *p = (char *)buf;
	char ch = *p;
	int ret;

	if ((ch == 'e') || (ch == 'E'))
		enable = 1;
	else if ((ch == 'd') || (ch == 'D'))
		enable = 0;
	else
		return -EINVAL;

	if (enable && wdt->timeout)
		return count;
	if (!enable && !wdt->timeout)
		return count;

	if (enable)
		wdt->timeout = wdt->org_timeout;
	else
		wdt->timeout = 0;

	if (wdt->org_timeout) {
		if (wdt->timeout) {
			ret = palmas_wdt_restart(&wdt->wdt_dev,
						wdt->timeout);
		if (ret < 0)
			dev_err(wdt->dev,
				"Watchdog not restarted %d\n", ret);

		} else {
			ret = palmas_wdt_stop(&wdt->wdt_dev);
			if (ret < 0)
				dev_err(wdt->dev, "wdt stop failed: %d\n", ret);
		}
	}
	return count;
}

static DEVICE_ATTR(watchdog_state, 0644, show_wdt_state, set_wdt_state);

static int palmas_wdt_probe(struct platform_device *pdev)
{
	struct palmas_platform_data *pdata;
	struct palmas_wdt *wdt;
	struct watchdog_device *wdt_dev;
	unsigned int regval;
	int ret;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	pdata = dev_get_platdata(pdev->dev.parent);
	if (pdata)
		wdt->watchdog_timer_initial_period =
			pdata->watchdog_timer_initial_period;
	if (!wdt->watchdog_timer_initial_period && pdev->dev.of_node) {
		u32 pval = 0;
		ret = of_property_read_u32(pdev->dev.of_node,
			"ti,watchdog-init-timeout", &pval);
		if (!ret)
			wdt->watchdog_timer_initial_period = pval;
		wdt->interrupt_mode = of_property_read_bool(pdev->dev.of_node,
					"ti,watchdog-interrupt-mode");
		if (wdt->interrupt_mode) {
			if (wdt->watchdog_timer_initial_period) {
				wdt->rtc_wakeup_period = wdt->watchdog_timer_initial_period;
				if (wdt->watchdog_timer_initial_period > 128)
					wdt->watchdog_timer_initial_period = 128;
			}
		} else {
			if (wdt->watchdog_timer_initial_period > 128)
				wdt->watchdog_timer_initial_period = 128;
		}
	}

	wdt->dev = &pdev->dev;
	wdt->palmas = dev_get_drvdata(pdev->dev.parent);
	wdt->irq = palmas_irq_get_virq(wdt->palmas, PALMAS_WDT_IRQ);
	wdt->org_timeout = wdt->watchdog_timer_initial_period;
	wdt_dev = &wdt->wdt_dev;

	wdt_dev->info = &palmas_wdt_info;
	wdt_dev->ops = &palmas_wdt_ops;
	wdt_dev->timeout = 128;
	wdt_dev->min_timeout = 1;
	wdt_dev->max_timeout = 128;
	watchdog_set_nowayout(wdt_dev, nowayout);
	watchdog_set_drvdata(wdt_dev, wdt);
	platform_set_drvdata(pdev, wdt);

	ret = request_threaded_irq(wdt->irq, NULL, palmas_wdt_irq,
			IRQF_ONESHOT | IRQF_EARLY_RESUME,
			dev_name(&pdev->dev), wdt);
	if (ret < 0) {
		dev_err(&pdev->dev, "request IRQ:%d failed, err = %d\n",
			 wdt->irq, ret);
		return ret;
	}

	ret = watchdog_register_device(wdt_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "watchdog registration failed: %d\n", ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_watchdog_state);
	if (ret < 0)
		dev_warn(&pdev->dev, "sysfs creation failed: %d\n", ret);

	ret = palmas_read(wdt->palmas, PALMAS_PMU_CONTROL_BASE,
			PALMAS_WATCHDOG, &regval);
	if (ret < 0) {
		dev_err(wdt->dev, "WATCHDOG read failed: %d\n", ret);
		goto scrub;
	}

	if (regval & PALMAS_WATCHDOG_LOCK) {
		dev_info(wdt->dev, "watchdog timer Locked\n");
		wdt->locked = true;
	} else {
		dev_info(wdt->dev, "watchdog timer unlocked\n");
		wdt->locked = false;
	}

	if (!wdt->locked) {
		ret = palmas_wdt_stop(wdt_dev);
		if (ret < 0) {
			dev_err(wdt->dev, "wdt stop failed: %d\n", ret);
			goto scrub;
		}
	}

	regval = (wdt->interrupt_mode) ? PALMAS_WATCHDOG_MODE : 0;
	ret = palmas_update_bits(wdt->palmas, PALMAS_PMU_CONTROL_BASE,
			PALMAS_WATCHDOG, PALMAS_WATCHDOG_MODE, regval);
	if (ret < 0) {
		dev_err(wdt->dev, "WATCHDOG update failed: %d\n", ret);
		goto scrub;
	}

	if (wdt->watchdog_timer_initial_period > 0) {
		dev_info(wdt->dev, "WATCHDOG starting with timeout %d\n",
				wdt->watchdog_timer_initial_period);
		ret = palmas_wdt_set_timeout(wdt_dev,
					wdt->watchdog_timer_initial_period);
		if (ret < 0) {
			dev_err(wdt->dev, "wdt set timeout failed: %d\n", ret);
			goto scrub;
		}
		if (!wdt->locked) {
			ret = palmas_wdt_start(wdt_dev);
			if (ret < 0) {
				dev_err(wdt->dev,
					"wdt start failed: %d\n", ret);
				goto scrub;
			}
		}
	}

	if (wdt->rtc_wakeup_period)
		alarmtimer_set_maximum_wakeup_interval_time(
				wdt->rtc_wakeup_period);

	device_set_wakeup_capable(&pdev->dev, 1);
	device_wakeup_enable(&pdev->dev);

	return 0;
scrub:
	free_irq(wdt->irq, wdt);
	watchdog_unregister_device(&wdt->wdt_dev);
	return ret;
}

static int palmas_wdt_remove(struct platform_device *pdev)
{
	struct palmas_wdt *wdt = platform_get_drvdata(pdev);

	palmas_wdt_stop(&wdt->wdt_dev);
	watchdog_unregister_device(&wdt->wdt_dev);
	free_irq(wdt->irq, wdt);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int palmas_wdt_suspend(struct device *dev)
{
	struct palmas_wdt *wdt = dev_get_drvdata(dev);
	int ret;

	if (device_may_wakeup(dev)) {
		enable_irq_wake(wdt->irq);
	} else if (wdt->timeout > 0) {
		ret = palmas_wdt_stop(&wdt->wdt_dev);
		if (ret < 0)
			dev_err(wdt->dev, "wdt stop failed: %d\n", ret);
	}
	return 0;
}

static int palmas_wdt_resume(struct device *dev)
{
	struct palmas_wdt *wdt = dev_get_drvdata(dev);
	int ret;

	if (device_may_wakeup(dev)) {
		disable_irq_wake(wdt->irq);
	} else if (wdt->timeout > 0) {
		ret = palmas_wdt_start(&wdt->wdt_dev);
		if (ret < 0)
			dev_err(wdt->dev, "wdt start failed: %d\n", ret);
	}
	return 0;
}
#endif

static const struct dev_pm_ops palmas_wdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(palmas_wdt_suspend, palmas_wdt_resume)
};

static struct of_device_id of_palmas_wdt[] = {
        { .compatible = "ti,palmas-wdt", },
        { .compatible = "ti,palmas-watchdog", },
        { /* end */ }
};
MODULE_DEVICE_TABLE(of, of_palmas_wdt);

static struct platform_driver palmas_wdt_driver = {
	.driver	= {
		.name	= "palmas-wdt",
		.owner	= THIS_MODULE,
		.pm = &palmas_wdt_pm_ops,
		.of_match_table = of_palmas_wdt,
	},
	.probe	= palmas_wdt_probe,
	.remove	= palmas_wdt_remove,
};

static int __init palmas_wdt_init(void)
{
	return platform_driver_register(&palmas_wdt_driver);
}
subsys_initcall(palmas_wdt_init);

static void __exit palmas_wdt_exit(void)
{
	platform_driver_unregister(&palmas_wdt_driver);
}
module_exit(palmas_wdt_exit);

MODULE_ALIAS("platform:palmas-wdt");
MODULE_DESCRIPTION("TI Palmas watchdog timer driver");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_LICENSE("GPL v2");
