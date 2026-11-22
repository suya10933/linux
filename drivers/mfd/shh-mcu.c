// SPDX-License-Identifier: GPL-2.0-only
/*
 * SHH board MCU (RP2354A) SPI driver
 */

#include <linux/backlight.h>
#include <linux/cache.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/mfd/shh-mcu.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/spi/spi.h>
#include <linux/crc8.h>
#include <linux/devm-helpers.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/wait.h>

#define SHH_MCU_MAX_SPEED_HZ	10000000
#define SHH_MCU_MSG_GAP_US	100

#define SHH_MCU_CRC8_POLY	0x31
#define SHH_MCU_CRC8_INIT	0xff

#define SHH_MCU_SYNC_PERIOD_MS	4	/* about 250hz */
#define SHH_MCU_MAX_ERRORS	16

DECLARE_CRC8_TABLE(shh_mcu_crc8_table);

struct shh_mcu_buf {
	__be32 magic;
	u8 ack;
	u8 data[SHH_MCU_DATA_SIZE];
	u8 crc;
} __packed;

struct shh_mcu {
	struct spi_device *spi;
	struct work_struct sync_work;
	struct hrtimer sync_timer;
	ktime_t sync_period;

	struct mutex lock;
	struct shh_mcu_buf buf_tx ____cacheline_aligned;
	struct shh_mcu_buf buf_rx ____cacheline_aligned;
	bool valid;

	unsigned int rx_error;
	unsigned int tx_error;

	unsigned char status[SHH_MCU_DATA_SIZE] ____cacheline_aligned;
	unsigned char ctrl[SHH_MCU_DATA_SIZE] ____cacheline_aligned;

	struct input_dev *pwrkey;
	struct input_dev *gamepad;
	struct gpio_chip lcd_reset_gc;
};

/* LCD Reset GPIO */
static int shh_mcu_lcd_reset_gpio_get_direction(struct gpio_chip *gc,
	unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static void shh_mcu_lcd_reset_gpio_set(struct gpio_chip *gc,
	unsigned int offset, int value)
{
	struct shh_mcu *mcu = gpiochip_get_data(gc);
	u8 t = value ? 1 : 0;

	mutex_lock(&mcu->lock);
	mcu->ctrl[SHH_MCU_LCD_RESET_OFFSET] = t;
	mutex_unlock(&mcu->lock);
}

static int shh_mcu_lcd_reset_gpio_get(struct gpio_chip *gc,
	unsigned int offset)
{
	struct shh_mcu *mcu = gpiochip_get_data(gc);
	int t;

	mutex_lock(&mcu->lock);
	t = mcu->ctrl[SHH_MCU_LCD_RESET_OFFSET];
	mutex_unlock(&mcu->lock);

	return t;
}

static int shh_mcu_lcd_reset_gpio_direction_output(struct gpio_chip *gc,
	unsigned int offset, int value)
{
	shh_mcu_lcd_reset_gpio_set(gc, offset, value);
	return 0;
}

/* LCD Brigheness */
static int shh_mcu_backlight_update(struct backlight_device *bl)
{
	struct shh_mcu *mcu = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	mutex_lock(&mcu->lock);
	mcu->ctrl[SHH_MCU_LCD_BRIGHTNESS_OFFSET] =
		brightness & SHH_MCU_LCD_BRIGHTNESS_MASK;
	mutex_unlock(&mcu->lock);

	return 0;
}

static const struct backlight_ops shh_mcu_bl = {
	.update_status = shh_mcu_backlight_update,
};

/* Power Event */
static void shh_mcu_handle_power_event(struct shh_mcu *mcu)
{
	u8 ev, last_ev;

	mutex_lock(&mcu->lock);
	ev = mcu->status[SHH_MCU_POWER_EVENT_OFFSET];
	last_ev = mcu->ctrl[SHH_MCU_POWER_EVENT_ACK_OFFSET];
	mcu->ctrl[SHH_MCU_POWER_EVENT_ACK_OFFSET] = ev;
	mutex_unlock(&mcu->lock);

	if (ev != last_ev) {
		if (ev == SHH_MCU_POWER_EVENT_PWRKEY) {
			input_report_key(mcu->pwrkey, KEY_POWER, 1);
			input_sync(mcu->pwrkey);
		} else {
			input_report_key(mcu->pwrkey, KEY_POWER, 0);
			input_sync(mcu->pwrkey);
		}
	}
}

static void shh_mcu_buf_claim_crc8(struct shh_mcu_buf *b)
{
	b->crc = crc8(shh_mcu_crc8_table, (const u8 *)b,
		      offsetof(struct shh_mcu_buf, crc), SHH_MCU_CRC8_INIT);
}

static bool shh_mcu_buf_valid_crc8(const struct shh_mcu_buf *b)
{
	return (b->crc == crc8(shh_mcu_crc8_table, (const u8 *)b,
			       offsetof(struct shh_mcu_buf, crc),
			       SHH_MCU_CRC8_INIT));
}

static bool shh_mcu_buf_valid(const struct shh_mcu_buf *b)
{
	if (shh_mcu_buf_valid_crc8(b) && b->magic == cpu_to_be32(SHH_MCU_MAGIC))
		return true;
	return false;
}

static int shh_mcu_buf_sync(struct shh_mcu *mcu)
{
	struct spi_transfer xfer = {
		.tx_buf = &mcu->buf_tx,
		.rx_buf = &mcu->buf_rx,
		.len = SHH_MCU_XFER_LENGTH,
	};
	int ret;

	mutex_lock(&mcu->lock);
	mcu->ctrl[SHH_MCU_CM4_STATUS_OFFSET] = (uint8_t)system_state;
	memcpy(mcu->buf_tx.data, mcu->ctrl, sizeof(mcu->ctrl));
	mutex_unlock(&mcu->lock);
	mcu->buf_tx.ack = mcu->rx_error ? 1 : 0;
	shh_mcu_buf_claim_crc8(&(mcu->buf_tx));

	ret = spi_sync_transfer(mcu->spi, &xfer, 1);
	if (!ret && !shh_mcu_buf_valid(&mcu->buf_rx))
		ret = -EBADMSG;
	if (ret) {
		if (++(mcu->rx_error) == SHH_MCU_MAX_ERRORS) {
			mutex_lock(&mcu->lock);
			mcu->valid = false;
			mutex_unlock(&mcu->lock);
			dev_warn(&mcu->spi->dev, "link lost\n");
		}
		return ret;
	}

	mutex_lock(&mcu->lock);
	memcpy(mcu->status, mcu->buf_rx.data, sizeof(mcu->status));
	mcu->valid = true;
	mutex_unlock(&mcu->lock);

	mcu->rx_error = 0;
	mcu->tx_error = mcu->buf_rx.ack ? mcu->tx_error + 1 : 0;

	return 0;
}

static void shh_mcu_sync_work(struct work_struct *work)
{
	struct shh_mcu *mcu = container_of(work, struct shh_mcu, sync_work);
	int ret;

	ret = shh_mcu_buf_sync(mcu);
	if (ret)
		dev_dbg_ratelimited(&mcu->spi->dev, "sync: %d\n", ret);
	else {
		shh_mcu_handle_power_event(mcu);
	}
}

static enum hrtimer_restart shh_mcu_sync_timer(struct hrtimer *t)
{
	struct shh_mcu *mcu = container_of(t, struct shh_mcu, sync_timer);

	queue_work(system_highpri_wq, &mcu->sync_work);
	hrtimer_forward_now(t, mcu->sync_period);

	return HRTIMER_RESTART;
}

static void shh_mcu_stop_timer(void *data)
{
	struct shh_mcu *mcu = data;

	hrtimer_cancel(&mcu->sync_timer);
}

static int shh_mcu_probe(struct spi_device *spi)
{
	struct backlight_properties props = { };
	struct backlight_device *bl;
	struct device *dev = &spi->dev;
	struct shh_mcu *mcu;
	int ret;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	crc8_populate_msb(shh_mcu_crc8_table, SHH_MCU_CRC8_POLY);

	mcu->buf_tx.magic = cpu_to_be32(SHH_MCU_MAGIC);

	/* mutex init */
	ret = devm_mutex_init(dev, &mcu->lock);
	if (ret)
		return ret;

	/* spi init */
	mcu->spi = spi;
	spi_set_drvdata(spi, mcu);

	spi->mode = (spi->mode & ~SPI_MODE_X_MASK) | SPI_MODE_1;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > SHH_MCU_MAX_SPEED_HZ)
		spi->max_speed_hz = SHH_MCU_MAX_SPEED_HZ;
	if (!spi->cs_inactive.value) {
		spi->cs_inactive.value = SHH_MCU_MSG_GAP_US;
		spi->cs_inactive.unit = SPI_DELAY_UNIT_USECS;
	}
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	/* initialize ctrl data */
	mcu->ctrl[SHH_MCU_CM4_STATUS_OFFSET] =
		SHH_MCU_CM4_STATUS_DEFAULT;
	mcu->ctrl[SHH_MCU_LCD_RESET_OFFSET] =
		SHH_MCU_LCD_RESET_DEFAULT;
	mcu->ctrl[SHH_MCU_LCD_BRIGHTNESS_OFFSET] =
		SHH_MCU_LCD_BRIGHTNESS_DEFAULT;

	/* backlight device init */
	props.type = BACKLIGHT_RAW;
	props.max_brightness = SHH_MCU_LCD_BRIGHTNESS_MAX;
	props.brightness = SHH_MCU_LCD_BRIGHTNESS_DEFAULT;
	props.power = BACKLIGHT_POWER_OFF;
	bl = devm_backlight_device_register(&spi->dev, "shh-mcu-bl",
					    &spi->dev, mcu,
					    &shh_mcu_bl,
					    &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	/* power key init */
	mcu->pwrkey = devm_input_allocate_device(dev);
	if (!mcu->pwrkey)
		return -ENOMEM;
	mcu->pwrkey->name = "shh-mcu powerkey";
	mcu->pwrkey->phys = "shh-mcu/input0";
	mcu->pwrkey->id.bustype = BUS_SPI;
	input_set_capability(mcu->pwrkey, EV_KEY, KEY_POWER);
	ret = input_register_device(mcu->pwrkey);
	if (ret)
		return dev_err_probe(dev, ret, "pwrkey init failed\n");

	/* lcd reset gpio init */
	mcu->lcd_reset_gc.label = "shh-mcu";
	mcu->lcd_reset_gc.parent = dev;
	mcu->lcd_reset_gc.owner = THIS_MODULE;
	mcu->lcd_reset_gc.base = -1;
	mcu->lcd_reset_gc.ngpio = 1;
	mcu->lcd_reset_gc.can_sleep = true;
	mcu->lcd_reset_gc.get_direction =
		shh_mcu_lcd_reset_gpio_get_direction;
	mcu->lcd_reset_gc.direction_output =
		shh_mcu_lcd_reset_gpio_direction_output;
	mcu->lcd_reset_gc.get = shh_mcu_lcd_reset_gpio_get;
	mcu->lcd_reset_gc.set = shh_mcu_lcd_reset_gpio_set;
	ret = devm_gpiochip_add_data(dev, &(mcu->lcd_reset_gc), mcu);
	if (ret)
		return dev_err_probe(dev, ret, "lcd_reset_gc failed\n");

	/* test sync */
	for (int i = 0; i < 3 && !(mcu->valid); ++i)
		shh_mcu_buf_sync(mcu);
	if (!(mcu->valid))
		dev_warn(dev, "no valid buf yet\n");

	/* hrtimer work init */
	ret = devm_work_autocancel(dev, &(mcu->sync_work), shh_mcu_sync_work);
	if (ret)
		return ret;
	mcu->sync_period = ms_to_ktime(SHH_MCU_SYNC_PERIOD_MS);
	hrtimer_init(&mcu->sync_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	mcu->sync_timer.function = shh_mcu_sync_timer;
	ret = devm_add_action_or_reset(dev, shh_mcu_stop_timer, mcu);
	if (ret)
		return ret;
	hrtimer_start(&(mcu->sync_timer), mcu->sync_period, HRTIMER_MODE_REL);

	dev_info(dev, "SHH MCU found, %u HZ\n", spi->max_speed_hz);

	return 0;
}

static void shh_mcu_shutdown(struct spi_device *spi)
{
	struct shh_mcu *mcu = spi_get_drvdata(spi);
	int i;

	hrtimer_cancel(&(mcu->sync_timer));
	disable_work_sync(&(mcu->sync_work));
	if ((system_state != SYSTEM_POWER_OFF) &&
	    (system_state != SYSTEM_RESTART) &&
	    (system_state != SYSTEM_HALT))
		return;

	for (i = 0; i < 10; ++i) {
		if (!shh_mcu_buf_sync(mcu) && i >= 1 && !(mcu->buf_rx.ack))
			break;
		usleep_range(1000, 2000);
	}
}

static const struct of_device_id shh_mcu_of_match[] = {
	{ .compatible = "shh,shh-mcu"},
	{ }
};
MODULE_DEVICE_TABLE(of, shh_mcu_of_match);

static struct spi_driver shh_mcu_driver = {
	.driver = {
		.name		= "shh-mcu",
		.of_match_table	= shh_mcu_of_match,
	},
	.probe = shh_mcu_probe,
	.shutdown = shh_mcu_shutdown,
};
module_spi_driver(shh_mcu_driver);

MODULE_DESCRIPTION("SHH board MCU(RP2354A) SPI driver");
MODULE_LICENSE("GPL");
