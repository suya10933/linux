// SPDX-License-Identifier: GPL-2.0-only
/*
 * SHH board MCU (RP2354A) SPI driver
 */


#include <linux/cache.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/shh-mcu.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/crc8.h>
#include <linux/devm-helpers.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

#define SHH_MCU_MAX_SPEED_HZ	10000000
#define SHH_MCU_MSG_GAP_US	5

#define SHH_MCU_CRC8_POLY	0x31
#define SHH_MCU_CRC8_INIT	0xff
#define SHH_MCU_CRC8_CHECK	0xf7

#define SHH_MCU_SYNC_PERIOD_MS	16	/* about 60hz */
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
	struct regmap *regmap;
	struct delayed_work sync_work;
	unsigned long sync_period;

	struct mutex lock;
	struct shh_mcu_buf *buf_rx;
	u8 tx_data[SHH_MCU_DATA_SIZE];
	bool valid;

	unsigned int rx_error;
	unsigned int tx_error;

	struct shh_mcu_buf buf_tx ____cacheline_aligned;
	struct shh_mcu_buf buf_rx_bank0 ____cacheline_aligned;
	struct shh_mcu_buf buf_rx_bank1 ____cacheline_aligned;
};

static void shh_mcu_buf_claim_crc8(struct shh_mcu_buf *b)
{
	b->crc = crc8(shh_mcu_crc8_table, (const u8 *)b,
		      offsetof(struct shh_mcu_buf, crc), SHH_MCU_CRC8_INIT);
}

static bool shh_mcu_buf_valid_crc8(const struct shh_mcu_buf *b)
{
	return (b->crc != crc8(shh_mcu_crc8_table, (const u8 *)b,
			       offsetof(struct shh_mcu_buf, crc), SHH_MCU_CRC8_INIT));
}

static bool shh_mcu_buf_valid(const struct shh_mcu_buf *b)
{
	if (b->magic != cpu_to_be32(SHH_MCU_MAGIC))
		return false;
	if (shh_mcu_buf_valid_crc8(b))
		return false;
	return true;
}

static int shh_mcu_buf_sync(struct shh_mcu *mcu)
{
	struct shh_mcu_buf *buf_rx_bank = (mcu->buf_rx == &(mcu->buf_rx_bank0)) ?
		&(mcu->buf_rx_bank1) : &(mcu->buf_rx_bank0);
	struct spi_transfer xfer = {
		.tx_buf = &mcu->buf_tx,
		.rx_buf = buf_rx_bank,
		.len = SHH_MCU_XFER_LENGTH,
	};
	int ret;

	mutex_lock(&mcu->lock);
	memcpy(mcu->buf_tx.data, mcu->tx_data, sizeof(mcu->tx_data));
	mutex_unlock(&mcu->lock);
	mcu->buf_tx.ack = mcu->rx_error ? 1 : 0;
	shh_mcu_buf_claim_crc8(&(mcu->buf_tx));

	ret = spi_sync_transfer(mcu->spi, &xfer, 1);
	if (!ret && !shh_mcu_buf_valid(buf_rx_bank))
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
	mcu->buf_rx = buf_rx_bank;
	mcu->valid = true;
	mutex_unlock(&mcu->lock);

	mcu->rx_error = 0;
	mcu->tx_error = buf_rx_bank->ack ? mcu->tx_error + 1 : 0;

	return 0;
}

static void shh_mcu_sync_work(struct work_struct *work)
{
	struct shh_mcu *mcu = container_of(to_delayed_work(work),
					   struct shh_mcu, sync_work);
	int ret;

	ret = shh_mcu_buf_sync(mcu);
	if (ret)
		dev_dbg_ratelimited(&mcu->spi->dev, "sync: %d\n", ret);

	queue_delayed_work(system_highpri_wq, &mcu->sync_work, mcu->sync_period);
}

static int shh_mcu_regmap_read(void *context, unsigned int reg, unsigned int *val)
{
	struct shh_mcu *mcu = context;

	if (reg >= SHH_MCU_TX_BASE) {
		*val = mcu->tx_data[reg - SHH_MCU_TX_BASE];
		return 0;
	}

	if (!mcu->valid)
		return -EIO;
	*val = mcu->buf_rx->data[reg - SHH_MCU_RX_BASE];
	return 0;
}

static int shh_mcu_regmap_write(void *context, unsigned int reg, unsigned int val)
{
	struct shh_mcu *mcu = context;
	mcu->tx_data[reg - SHH_MCU_TX_BASE] = val;

	return 0;
}

static void shh_mcu_regmap_lock(void *arg)
{
	struct shh_mcu *mcu = arg;

	mutex_lock(&mcu->lock);
}

static void shh_mcu_regmap_unlock(void *arg)
{
	struct shh_mcu *mcu = arg;

	mutex_unlock(&mcu->lock);
}

static const struct regmap_range shh_mcu_rd_ranges[] = {
	regmap_reg_range(SHH_MCU_RX_BASE, SHH_MCU_RX_BASE + SHH_MCU_DATA_SIZE - 1),
	regmap_reg_range(SHH_MCU_TX_BASE, SHH_MCU_TX_BASE + SHH_MCU_DATA_SIZE - 1),
};

static const struct regmap_range shh_mcu_wr_ranges[] = {
	regmap_reg_range(SHH_MCU_TX_BASE, SHH_MCU_TX_BASE + SHH_MCU_DATA_SIZE - 1),
};

static const struct regmap_access_table shh_mcu_wr_table = {
	.yes_ranges = shh_mcu_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(shh_mcu_wr_ranges),
};

static const struct regmap_access_table shh_mcu_rd_table = {
	.yes_ranges = shh_mcu_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(shh_mcu_rd_ranges),
};

static const struct regmap_config shh_mcu_regmap_config = {
	.reg_read = shh_mcu_regmap_read,
	.reg_write = shh_mcu_regmap_write,
	.reg_bits = 6,
	.val_bits = 8,
	.max_register = SHH_MCU_TX_BASE + SHH_MCU_TX_SIZE - 1,
	.cache_type = REGCACHE_NONE,
	.max_raw_read = SHH_MCU_DATA_SIZE,
	.max_raw_write = SHH_MCU_DATA_SIZE,
	.rd_table = &shh_mcu_rd_table,
	.wr_table = &shh_mcu_wr_table,
};

static int shh_mcu_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct shh_mcu *mcu;
	int ret;
	struct regmap_config cfg = shh_mcu_regmap_config;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	crc8_populate_msb(shh_mcu_crc8_table, SHH_MCU_CRC8_POLY);

	mcu->buf_rx = &mcu->buf_rx_bank0;
	mcu->buf_tx.magic = cpu_to_be32(SHH_MCU_MAGIC);

	/* mutex n delayed work init */
	ret = devm_mutex_init(dev, &mcu->lock);
	if (ret)
		return ret;
	mcu->sync_period = msecs_to_jiffies(SHH_MCU_SYNC_PERIOD_MS);
	ret = devm_delayed_work_autocancel(dev, &mcu->sync_work, shh_mcu_sync_work);

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

	/* test sync */
	for (int i = 0; i < 3 && !(mcu->valid); ++i)
		shh_mcu_buf_sync(mcu);
	if (!(mcu->valid))
		dev_warn(dev, "no valid buf yet\n");

	/* regmap init */
	cfg.lock = shh_mcu_regmap_lock;
	cfg.unlock = shh_mcu_regmap_unlock;
	cfg.lock_arg = mcu;
	cfg.can_sleep = true;
	mcu->regmap = devm_regmap_init(dev, NULL, mcu,
				       &cfg);

	if (IS_ERR(mcu->regmap))
		return dev_err_probe(dev, PTR_ERR(mcu->regmap),
				     "regmap init failed\n");

	queue_delayed_work(system_highpri_wq, &mcu->sync_work, mcu->sync_period);

	dev_info(dev, "SHH MCU found, %u HZ\n", spi->max_speed_hz);

	return 0;
}

static void shh_mcu_shutdown (struct spi_device *spi)
{
	struct shh_mcu *mcu = spi_get_drvdata(spi);
	int i;

	disable_delayed_work_sync(&mcu->sync_work);
	if (system_state != SYSTEM_POWER_OFF)
		return;

	mutex_lock(&mcu->lock);
	/* TBD : send shutdown signal to mcu */
	//mcu->tx_data[SHH_MCU_POWER_OFFSET] |= SHH_MCU_POWER_SHUTDOWN;
	mutex_unlock(&mcu->lock);

	for (i = 0; i < 4; ++i)
		if (!shh_mcu_buf_sync(mcu) && i >= 1 && !(mcu->buf_rx->ack))
			break;
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
