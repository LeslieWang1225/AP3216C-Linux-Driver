// SPDX-License-Identifier: GPL-2.0-only
/* AP3216C 接近与环境光传感器驱动 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

/* 系统寄存器及 PS / ALS 数据寄存器 */
#define CMD_SYSMODE 0x00
#define CMD_INTSTAT 0x01
#define CMD_INTCLRM 0x02
#define CMD_ALSDATAL 0x0c
#define CMD_ALSDATAH 0x0d
#define CMD_PSDATAL 0x0e
#define CMD_PSDATAH 0x0f

/* PS 配置寄存器 */
#define CMD_PSLEDCTL 0x21
#define CMD_PSLEDWT 0x24
#define CMD_PSLTHDL 0x2a
#define CMD_PSLTHDH 0x2b
#define CMD_PSHTHDL 0x2c
#define CMD_PSHTHDH 0x2d

/* 工作模式 */
#define MODE_PWDOWN 0x00
#define MODE_PSONLY 0x02
#define MODE_PSALS 0x03
#define WT_INTV_MIN 0x00

struct ap3216c_dev
{
	const char *name;
	struct input_dev *input_dev;
	struct i2c_client *client;

	int irq;
	struct work_struct work;
	struct mutex mutex;
	bool near;
	bool pm_held;

	/* sysfs 配置 */
	int ps_threshold_low;
	int ps_threshold_high;
	int ps_measure_interval;
	int ps_led_current_ratio;
};

/* 调用方负责串行化寄存器访问。 */
static int i2c_read(unsigned char cmd, struct i2c_client *client)
{
	int ret;
	unsigned char data;
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &cmd,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = &data,
		},
	};

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret != (int)ARRAY_SIZE(msg)) {
		dev_err(&client->dev, "I2C read 0x%02x failed: %d\n", cmd, ret);
		return ret < 0 ? ret : -EIO;
	}

	return data;
}

static int i2c_write(unsigned char cmd, unsigned char val, struct i2c_client *client)
{
	int ret;
	unsigned char data[] = { cmd, val };
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = data,
		},
	};

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret != (int)ARRAY_SIZE(msg)) {
		dev_err(&client->dev, "I2C write 0x%02x failed: %d\n", cmd, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

/* PS 阈值由低两位寄存器和高八位寄存器组成。 */
static int ap3216c_write_threshold(struct ap3216c_dev *pdata, unsigned char reg, int value)
{
	int ret;

	ret = i2c_write(reg, value & 0x03, pdata->client);
	if (ret < 0)
		return ret;

	return i2c_write(reg + 1, (value >> 2) & 0xff, pdata->client);
}

/* PS 下阈值。 */
static ssize_t ps_threshold_low_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int l = 0;
	int h = 0;
	int val = 0;

	mutex_lock(&pdata->mutex);
	l = i2c_read(CMD_PSLTHDL, pdata->client);
	h = i2c_read(CMD_PSLTHDH, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (l < 0 || h < 0)
		return -EIO;

	val = (h << 2) | (l & 0x03);
	return sprintf(buf, "%d\n", val);
}

static ssize_t ps_threshold_low_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < 0 || val > 0x3ff)
		return -EINVAL;

	mutex_lock(&pdata->mutex);
	if (val > pdata->ps_threshold_high) {
		mutex_unlock(&pdata->mutex);
		return -EINVAL;
	}

	ret = ap3216c_write_threshold(pdata, CMD_PSLTHDL, val);
	if (!ret)
		pdata->ps_threshold_low = val;
	mutex_unlock(&pdata->mutex);

	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(ps_threshold_low);

/* PS 上阈值。 */
static ssize_t ps_threshold_high_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int l = 0;
	int h = 0;
	int val = 0;

	mutex_lock(&pdata->mutex);
	l = i2c_read(CMD_PSHTHDL, pdata->client);
	h = i2c_read(CMD_PSHTHDH, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (l < 0 || h < 0)
		return -EIO;

	val = (h << 2) | (l & 0x03);
	return sprintf(buf, "%d\n", val);
}

static ssize_t ps_threshold_high_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < 0 || val > 0x3ff)
		return -EINVAL;

	mutex_lock(&pdata->mutex);
	if (val < pdata->ps_threshold_low) {
		mutex_unlock(&pdata->mutex);
		return -EINVAL;
	}

	ret = ap3216c_write_threshold(pdata, CMD_PSHTHDL, val);
	if (!ret)
		pdata->ps_threshold_high = val;
	mutex_unlock(&pdata->mutex);

	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(ps_threshold_high);

/* 远离时的测量周期，写入值按毫秒换算为寄存器档位。 */
static ssize_t ps_measure_interval_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int wait;
	int interval;

	mutex_lock(&pdata->mutex);
	wait = pdata->ps_measure_interval;
	mutex_unlock(&pdata->mutex);

	/* 默认平均次数下，每档对应约 112.5 ms。 */
	interval = (225 * (wait + 1) + 1) / 2;
	return sprintf(buf, "%d\n", interval);
}

static ssize_t ps_measure_interval_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int interval;
	int waitt;
	int ret = 0;

	if (kstrtoint(buf, 10, &interval) < 0)
		return -EINVAL;
	if (interval < 113 || interval > 7200)
		return -EINVAL;

	waitt = (interval * 2 + 112) / 225 - 1;

	mutex_lock(&pdata->mutex);
	if (!pdata->near)
		ret = i2c_write(CMD_PSLEDWT, waitt, pdata->client);
	if (!ret)
		pdata->ps_measure_interval = waitt;
	mutex_unlock(&pdata->mutex);

	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(ps_measure_interval);

/* LED 电流比例档位。 */
static ssize_t ps_led_current_ratio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int val = 0;

	mutex_lock(&pdata->mutex);
	val = i2c_read(CMD_PSLEDCTL, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (val < 0)
		return -EIO;

	return sprintf(buf, "%d\n", val & 0x03);
}

static ssize_t ps_led_current_ratio_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int ratio = 0;
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &ratio) < 0)
		return -EINVAL;
	if (ratio < 0 || ratio > 3)
		return -EINVAL;

	mutex_lock(&pdata->mutex);
	val = i2c_read(CMD_PSLEDCTL, pdata->client);
	if (val < 0) {
		mutex_unlock(&pdata->mutex);
		return -EIO;
	}

	val = (val & ~0x03) | ratio;
	ret = i2c_write(CMD_PSLEDCTL, val, pdata->client);
	if (!ret)
		pdata->ps_led_current_ratio = ratio;
	mutex_unlock(&pdata->mutex);

	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(ps_led_current_ratio);

/* PS 原始数据。 */
static ssize_t ps_raw_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int data_l = 0;
	int data_h = 0;
	int data = 0;

	mutex_lock(&pdata->mutex);
	data_l = i2c_read(CMD_PSDATAL, pdata->client);
	data_h = i2c_read(CMD_PSDATAH, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (data_l < 0 || data_h < 0)
		return -EIO;
	if (data_l & 0x40 || data_h & 0x40)
		return sprintf(buf, "Invalid data.\n");

	data = ((data_h & 0x3f) << 4) | (data_l & 0x0f);
	return sprintf(buf, "%d\n", data);
}

static DEVICE_ATTR_RO(ps_raw_data);

/* PS 属性组。 */
static struct attribute *ps_group_attrs[] = {
	&dev_attr_ps_threshold_low.attr,
	&dev_attr_ps_threshold_high.attr,
	&dev_attr_ps_measure_interval.attr,
	&dev_attr_ps_led_current_ratio.attr,
	&dev_attr_ps_raw_data.attr,
	NULL,
};

static const struct attribute_group ps_group = {
	.name = "proximity",
	.attrs = ps_group_attrs,
};

/* ALS 原始数据与属性组。 */
static ssize_t als_raw_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int data_l = 0;
	int data_h = 0;
	int data = 0;

	mutex_lock(&pdata->mutex);
	data_l = i2c_read(CMD_ALSDATAL, pdata->client);
	data_h = i2c_read(CMD_ALSDATAH, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (data_l < 0 || data_h < 0)
		return -EIO;

	data = (data_h << 8) | data_l;
	return sprintf(buf, "%d\n", data);
}

static DEVICE_ATTR_RO(als_raw_data);

static struct attribute *als_group_attrs[] = {
	&dev_attr_als_raw_data.attr,
	NULL,
};

static const struct attribute_group als_group = {
	.name = "ambient_light",
	.attrs = als_group_attrs,
};

/* 中断上半部只调度工作，I2C 访问在可睡眠的工作队列中完成。 */
static irqreturn_t ap3216c_irq_handler(int irq, void *dev_id)
{
	struct ap3216c_dev *pdata = dev_id;
	schedule_work(&pdata->work);
	return IRQ_HANDLED;
}

static void ap3216c_work_callback(struct work_struct *work)
{
	struct ap3216c_dev *pdata = container_of(work, struct ap3216c_dev, work);
	struct device *dev = &pdata->client->dev;
	int data_low, data_high, int_status, after_clear, ret;
	bool near, pm_held;

	mutex_lock(&pdata->mutex);
	int_status = i2c_read(CMD_INTSTAT, pdata->client);
	if (int_status < 0)
		goto unlock;

	/* 写 1 清除已读取的 ALS/PS 中断标志。 */
	if (int_status & 0x03) {
		ret = i2c_write(CMD_INTSTAT, int_status & 0x03, pdata->client);
		if (ret < 0)
			goto unlock;

		/* 保留已验证的清中断回读时序。 */
		after_clear = i2c_read(CMD_INTSTAT, pdata->client);
		if (after_clear >= 0)
			dev_dbg(dev, "interrupt status: 0x%02x -> 0x%02x\n", int_status, after_clear);
	}

	if (!(int_status & 0x02))
		goto unlock;

	data_low = i2c_read(CMD_PSDATAL, pdata->client);
	data_high = i2c_read(CMD_PSDATAH, pdata->client);
	if (data_low < 0 || data_high < 0)
		goto unlock;

	/* bit6 表示数据无效，低字节 bit7 表示接近。 */
	if ((data_low & 0x40) || (data_high & 0x40))
		goto unlock;

	near = !!(data_low & 0x80);
	if (near == pdata->near)
		goto unlock;

	pdata->near = near;
	pm_held = pdata->pm_held;
	mutex_unlock(&pdata->mutex);

	/* Runtime PM 回调也需要 mutex，因此在锁外更新电源状态。 */
	if (near) {
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			pm_runtime_put_noidle(dev);
			dev_err(dev, "runtime resume failed: %d\n", ret);
		} else {
			mutex_lock(&pdata->mutex);
			pdata->pm_held = true;
			mutex_unlock(&pdata->mutex);
		}
	} else if (pm_held) {
		pm_runtime_mark_last_busy(dev);
		ret = pm_runtime_put_autosuspend(dev);
		if (ret < 0)
			dev_err(dev, "runtime suspend request failed: %d\n", ret);
		mutex_lock(&pdata->mutex);
		pdata->pm_held = false;
		mutex_unlock(&pdata->mutex);
	}

	input_report_switch(pdata->input_dev, SW_FRONT_PROXIMITY, near);
	input_sync(pdata->input_dev);
	return;

unlock:
	mutex_unlock(&pdata->mutex);
}

/* 恢复用户配置的 PS 阈值、周期和 LED 电流。 */
static int ap3216c_reg_config(struct ap3216c_dev *pdata)
{
	int ret = 0;
	int val = 0;

	ret = ap3216c_write_threshold(pdata, CMD_PSLTHDL, pdata->ps_threshold_low);
	if (ret < 0)
		return ret;

	ret = ap3216c_write_threshold(pdata, CMD_PSHTHDL, pdata->ps_threshold_high);
	if (ret < 0)
		return ret;

	ret = i2c_write(CMD_PSLEDWT, pdata->ps_measure_interval, pdata->client);
	if (ret < 0)
		return ret;

	val = i2c_read(CMD_PSLEDCTL, pdata->client);
	if (val < 0)
		return val;

	val = (val & ~0x03) | pdata->ps_led_current_ratio;
	return i2c_write(CMD_PSLEDCTL, val, pdata->client);
}

/* 软复位后进入 PS+ALS 模式。 */
static int ap3216c_init(struct ap3216c_dev *pdata)
{
	int ret = 0;

	/* 软件复位后等待芯片就绪。 */
	ret = i2c_write(CMD_SYSMODE, 0x04, pdata->client);
	if (ret < 0)
		return ret;

	msleep(20);
	ret = i2c_write(CMD_SYSMODE, MODE_PSALS, pdata->client);
	if (ret < 0)
		return ret;

	/* 写 1 清除中断标志。 */
	ret = i2c_write(CMD_INTCLRM, 0x01, pdata->client);
	if (ret < 0)
		return ret;

	ret = ap3216c_reg_config(pdata);
	if (ret < 0)
		return ret;

	/* 清除初始化期间产生的中断。 */
	return i2c_write(CMD_INTSTAT, 0x03, pdata->client);
}

static int ap3216c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;
	struct ap3216c_dev *pdata;

	pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->client = client;
	pdata->irq = client->irq;
	pdata->name = "ap3216c-proximity";
	if (client->dev.of_node)
		of_property_read_string(client->dev.of_node, "label", &pdata->name);

	if (pdata->irq <= 0) {
		dev_err(&client->dev, "missing AP3216C interrupt\n");
		return -EINVAL;
	}

	i2c_set_clientdata(client, pdata);
	mutex_init(&pdata->mutex);
	INIT_WORK(&pdata->work, ap3216c_work_callback);

	/* 默认阈值和测量参数。 */
	pdata->ps_threshold_high = 100;
	pdata->ps_threshold_low = 50;
	pdata->ps_measure_interval = 0x04;
	pdata->ps_led_current_ratio = 3;

	ret = ap3216c_init(pdata);
	if (ret < 0) {
		dev_err(&client->dev, "chip initialization failed: %d\n", ret);
		goto err_powerdown;
	}

	/* Input 设备在解绑时自动注销。 */
	pdata->input_dev = devm_input_allocate_device(&client->dev);
	if (!pdata->input_dev) {
		ret = -ENOMEM;
		goto err_powerdown;
	}

	pdata->input_dev->name = pdata->name;
	pdata->input_dev->id.bustype = BUS_I2C;
	input_set_capability(pdata->input_dev, EV_SW, SW_FRONT_PROXIMITY);
	ret = input_register_device(pdata->input_dev);
	if (ret < 0)
		goto err_powerdown;

	/* 注册传感器属性。 */
	ret = sysfs_create_group(&client->dev.kobj, &ps_group);
	if (ret < 0)
		goto err_powerdown;

	ret = sysfs_create_group(&client->dev.kobj, &als_group);
	if (ret < 0)
		goto err_ps_group;

	/* 初始为较慢的 PS+ALS 模式。 */
	pm_runtime_set_autosuspend_delay(&client->dev, 500);
	pm_runtime_use_autosuspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_enable(&client->dev);

	/* 资源就绪后再允许中断。 */
	ret = request_irq(pdata->irq, ap3216c_irq_handler, IRQF_TRIGGER_FALLING, pdata->name, pdata);
	if (ret < 0)
		goto err_pm;

	dev_info(&client->dev, "AP3216C ready, irq=%d\n", pdata->irq);
	return 0;

err_pm:
	pm_runtime_disable(&client->dev);
	pm_runtime_dont_use_autosuspend(&client->dev);
	sysfs_remove_group(&client->dev.kobj, &als_group);
err_ps_group:
	sysfs_remove_group(&client->dev.kobj, &ps_group);
err_powerdown:
	i2c_write(CMD_SYSMODE, MODE_PWDOWN, client);
	i2c_set_clientdata(client, NULL);
	return ret;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct ap3216c_dev *pdata = i2c_get_clientdata(client);

	/* 先停止中断和 work，再由 devm 回收资源。 */
	free_irq(pdata->irq, pdata);
	cancel_work_sync(&pdata->work);

	sysfs_remove_group(&client->dev.kobj, &als_group);
	sysfs_remove_group(&client->dev.kobj, &ps_group);

	if (pdata->pm_held)
		pm_runtime_put_noidle(&client->dev);
	pm_runtime_disable(&client->dev);
	pm_runtime_dont_use_autosuspend(&client->dev);

	i2c_write(CMD_SYSMODE, MODE_PWDOWN, client);
	i2c_set_clientdata(client, NULL);

	return 0;
}

/* Runtime suspend 保持低频巡检；resume 切为高速 PS。 */
static int __maybe_unused ap3216c_runtime_suspend(struct device *dev)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int ret;

	mutex_lock(&pdata->mutex);
	ret = i2c_write(CMD_PSLEDWT, pdata->ps_measure_interval, pdata->client);
	if (!ret)
		ret = i2c_write(CMD_SYSMODE, MODE_PSALS, pdata->client);
	mutex_unlock(&pdata->mutex);

	return ret;
}

static int __maybe_unused ap3216c_runtime_resume(struct device *dev)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int ret;

	mutex_lock(&pdata->mutex);
	ret = i2c_write(CMD_PSLEDWT, WT_INTV_MIN, pdata->client);
	if (!ret)
		ret = i2c_write(CMD_SYSMODE, MODE_PSONLY, pdata->client);
	mutex_unlock(&pdata->mutex);

	return ret;
}

/* 系统休眠时完全关闭芯片，恢复时重建运行模式。 */
static int __maybe_unused ap3216c_suspend(struct device *dev)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int ret;

	disable_irq(pdata->irq);
	cancel_work_sync(&pdata->work);

	mutex_lock(&pdata->mutex);
	ret = i2c_write(CMD_SYSMODE, MODE_PWDOWN, pdata->client);
	mutex_unlock(&pdata->mutex);

	if (ret < 0)
		enable_irq(pdata->irq);

	return ret;
}

static int __maybe_unused ap3216c_resume(struct device *dev)
{
	struct ap3216c_dev *pdata = (struct ap3216c_dev *)dev->driver_data;
	int ret;

	mutex_lock(&pdata->mutex);
	ret = i2c_write(CMD_SYSMODE, MODE_PSALS, pdata->client);
	if (!ret)
		ret = ap3216c_reg_config(pdata);
	if (!ret && pdata->pm_held) {
		ret = i2c_write(CMD_PSLEDWT, WT_INTV_MIN, pdata->client);
		if (!ret)
			ret = i2c_write(CMD_SYSMODE, MODE_PSONLY, pdata->client);
	}
	if (!ret)
		ret = i2c_write(CMD_INTSTAT, 0x03, pdata->client);
	mutex_unlock(&pdata->mutex);

	enable_irq(pdata->irq);
	return ret;
}

static const struct dev_pm_ops ap3216c_pm_ops = {
	SET_RUNTIME_PM_OPS(ap3216c_runtime_suspend, ap3216c_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(ap3216c_suspend, ap3216c_resume)
};

static const struct of_device_id ap3216c_match_table[] = {
	{ .compatible = "lite-on,ap3216c", },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, ap3216c_match_table);

static const struct i2c_device_id ap3216c_id_table[] = {
	{ "ap3216c", 0 },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id_table);

static struct i2c_driver ap3216c_drv = {
	.driver = {
		.name = "ap3216c-driver",
		.of_match_table = ap3216c_match_table,
		.pm = &ap3216c_pm_ops,
	},
	.probe = ap3216c_probe,
	.remove = ap3216c_remove,
	.id_table = ap3216c_id_table,
};

module_i2c_driver(ap3216c_drv);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leslie Wang");
MODULE_DESCRIPTION("AP3216C proximity and ambient light driver");
