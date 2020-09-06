// SPDX-License-Identifier: GPL-2.0+
/*
 * Clock vibrator driver
 *
 * Copyright (c) 2019 Brian Masney <masneyb@onstation.org>
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

struct clk_vibrator {
	struct input_dev *input;
	struct mutex mutex;
	struct work_struct worker;
	struct regulator *vcc;
	struct clk *clk;
	u32 clk_rate;
	struct gpio_desc *enable_gpio;
	u16 magnitude;
	bool enabled;
};

static int clk_vibrator_start(struct clk_vibrator *vibrator)
{
	int ret;

	mutex_lock(&vibrator->mutex);

	if (!vibrator->enabled) {
		ret = clk_set_rate(vibrator->clk, vibrator->clk_rate);
		if (ret) {
			dev_err(&vibrator->input->dev,
				"Failed to set clock rate: %d\n", ret);
			goto unlock;
		}

		ret = clk_prepare_enable(vibrator->clk);
		if (ret) {
			dev_err(&vibrator->input->dev,
				"Failed to enable clock: %d\n", ret);
			goto unlock;
		}

		ret = regulator_enable(vibrator->vcc);
		if (ret) {
			dev_err(&vibrator->input->dev,
				"Failed to enable regulator: %d\n", ret);
			clk_disable(vibrator->clk);
			goto unlock;
		}

		gpiod_set_value_cansleep(vibrator->enable_gpio, 1);

		vibrator->enabled = true;
	}

	ret = clk_set_duty_cycle(vibrator->clk, vibrator->magnitude, 0xffff);

unlock:
	mutex_unlock(&vibrator->mutex);

	return ret;
}

static void clk_vibrator_stop(struct clk_vibrator *vibrator)
{
	mutex_lock(&vibrator->mutex);

	if (vibrator->enabled) {
		gpiod_set_value_cansleep(vibrator->enable_gpio, 0);
		regulator_disable(vibrator->vcc);
		clk_disable(vibrator->clk);
		vibrator->enabled = false;
	}

	mutex_unlock(&vibrator->mutex);
}

static void clk_vibrator_worker(struct work_struct *work)
{
	struct clk_vibrator *vibrator = container_of(work,
						     struct clk_vibrator,
						     worker);

	if (vibrator->magnitude)
		clk_vibrator_start(vibrator);
	else
		clk_vibrator_stop(vibrator);
}

static int clk_vibrator_play_effect(struct input_dev *dev, void *data,
				    struct ff_effect *effect)
{
	struct clk_vibrator *vibrator = input_get_drvdata(dev);

	mutex_lock(&vibrator->mutex);

	if (effect->u.rumble.strong_magnitude > 0)
		vibrator->magnitude = effect->u.rumble.strong_magnitude;
	else
		vibrator->magnitude = effect->u.rumble.weak_magnitude;

	mutex_unlock(&vibrator->mutex);

	schedule_work(&vibrator->worker);

	return 0;
}

static void clk_vibrator_close(struct input_dev *input)
{
	struct clk_vibrator *vibrator = input_get_drvdata(input);

	cancel_work_sync(&vibrator->worker);
	clk_vibrator_stop(vibrator);
}

static int clk_vibrator_probe(struct platform_device *pdev)
{
	struct clk_vibrator *vibrator;
	int ret;

	vibrator = devm_kzalloc(&pdev->dev, sizeof(*vibrator), GFP_KERNEL);
	if (!vibrator)
		return -ENOMEM;

	vibrator->input = devm_input_allocate_device(&pdev->dev);
	if (!vibrator->input)
		return -ENOMEM;

	vibrator->vcc = devm_regulator_get(&pdev->dev, "vcc");
	if (IS_ERR(vibrator->vcc)) {
		if (PTR_ERR(vibrator->vcc) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get regulator: %ld\n",
				PTR_ERR(vibrator->vcc));
		return PTR_ERR(vibrator->vcc);
	}

	vibrator->enable_gpio = devm_gpiod_get(&pdev->dev, "enable",
					       GPIOD_OUT_LOW);
	if (IS_ERR(vibrator->enable_gpio)) {
		if (PTR_ERR(vibrator->enable_gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get enable gpio: %ld\n",
				PTR_ERR(vibrator->enable_gpio));
		return PTR_ERR(vibrator->enable_gpio);
	}

	vibrator->clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(vibrator->clk)) {
		if (PTR_ERR(vibrator->clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"Failed to lookup core clock: %ld\n",
				PTR_ERR(vibrator->clk));
		return PTR_ERR(vibrator->clk);
	}

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				   &vibrator->clk_rate);
	if (ret) {
		dev_err(&pdev->dev, "Cannot read clock-frequency: %d\n", ret);
		return ret;
	}

	vibrator->enabled = false;
	mutex_init(&vibrator->mutex);
	INIT_WORK(&vibrator->worker, clk_vibrator_worker);

	vibrator->input->name = "clk-vibrator";
	vibrator->input->id.bustype = BUS_HOST;
	vibrator->input->close = clk_vibrator_close;

	input_set_drvdata(vibrator->input, vibrator);
	input_set_capability(vibrator->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(vibrator->input, NULL,
				      clk_vibrator_play_effect);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create ff memless: %d", ret);
		return ret;
	}

	ret = input_register_device(vibrator->input);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register input device: %d", ret);
		return ret;
	}

	platform_set_drvdata(pdev, vibrator);

	return 0;
}

static int __maybe_unused clk_vibrator_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct clk_vibrator *vibrator = platform_get_drvdata(pdev);

	cancel_work_sync(&vibrator->worker);

	if (vibrator->enabled)
		clk_vibrator_stop(vibrator);

	return 0;
}

static int __maybe_unused clk_vibrator_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct clk_vibrator *vibrator = platform_get_drvdata(pdev);

	if (vibrator->enabled)
		clk_vibrator_start(vibrator);

	return 0;
}

static SIMPLE_DEV_PM_OPS(clk_vibrator_pm_ops, clk_vibrator_suspend,
			 clk_vibrator_resume);

static const struct of_device_id clk_vibrator_of_match[] = {
	{ .compatible = "clk-vibrator" },
	{},
};
MODULE_DEVICE_TABLE(of, clk_vibrator_of_match);

static struct platform_driver clk_vibrator_driver = {
	.probe	= clk_vibrator_probe,
	.driver	= {
		.name = "clk-vibrator",
		.pm = &clk_vibrator_pm_ops,
		.of_match_table = of_match_ptr(clk_vibrator_of_match),
	},
};
module_platform_driver(clk_vibrator_driver);

MODULE_AUTHOR("Brian Masney <masneyb@onstation.org>");
MODULE_DESCRIPTION("Clock vibrator driver");
MODULE_LICENSE("GPL");
