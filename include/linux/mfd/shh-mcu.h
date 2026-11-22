/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_SHH_MCU_H
#define __LINUX_MFD_SHH_MCU_H

#include <linux/bits.h>

/* shh_mcu_status (26 bytes)
 *	0x00-0x00	Power Event
 *	0x01-0x01	Power Status
 *	0x02-0x02	Battery Capacity
 *	0x03-0x06	Buttons
 *	0x07-0x0A	Joysticks
 *	0x0B-0x0F	Touch
 *	0x10-0x19	Reserved
 * shh_mcu_ctrl (26 bytes)
 *	0x00-0x00	Cm4 Status
 *	0x01-0x01	LCD Reset
 *	0x02-0x02	LCD Brightness
 *	0x03-0x03	Power Event Ack
 *	0x04-0x19	Reserved
 */

/* spi buffer---------------------------------
 * rx_buffer
 *	0x00-0x03 - magic
 *	0x04-0x04 - ack
 *	0x05-0x1E - rx_data
 *	0x1F-0x1F - crc
 * tx_buffer
 *	0x00-0x03 - magic
 *	0x04-0x04 - ack
 *	0x05-0x1E - tx_data
 *	0x1F-0x1F - crc
 */

#define SHH_MCU_RAM_SIZE		64
#define SHH_MCU_XFER_LENGTH		32
#define SHH_MCU_ADDR_MASK		GENMASK(4, 0)
#define SHH_MCU_DATA_SIZE		26
#define SHH_MCU_MAGIC			0xDEADBEEF

#define SHH_MCU_STATUS_BASE		0x00
#define SHH_MCU_STATUS_SIZE		26

#define SHH_MCU_POWER_EVENT_OFFSET	0x00
#define SHH_MCU_POWER_EVENT_PWRKEY	0x0A
#define SHH_MCU_POWER_STATUS_OFFSET	0x01
#define SHH_MCU_BATTERY_CAPACITY_OFFSET	0x02
#define SHH_MCU_BUTTONS_OFFSET		0x03
#define SHH_MCU_JOYSTICKS_OFFSET	0x07
#define SHH_MCU_TOUCH_OFFSET		0x0B

#define SHH_MCU_CTRL_BASE		0x20
#define SHH_MCU_CTRL_SIZE		26

#define SHH_MCU_CM4_STATUS_OFFSET	0x00
#define SHH_MCU_CM4_STATUS_BOOTING	0x0A
#define SHH_MCU_CM4_STATUS_IDLE		0x04
#define SHH_MCU_CM4_STATUS_DEFAULT	0x0A
#define SHH_MCU_LCD_RESET_OFFSET	0x01
#define SHH_MCU_LCD_RESET_ACTIVE	0x00
#define SHH_MCU_LCD_RESET_DEACTIVE	0x01
#define SHH_MCU_LCD_RESET_DEFAULT	SHH_MCU_LCD_RESET_ACTIVE
#define SHH_MCU_LCD_BRIGHTNESS_OFFSET	0x02
#define SHH_MCU_LCD_BRIGHTNESS_MASK	0x0F
#define SHH_MCU_LCD_BRIGHTNESS_MAX	0x0F
#define SHH_MCU_LCD_BRIGHTNESS_DEFAULT	0x00
#define SHH_MCU_POWER_EVENT_ACK_OFFSET	0x03

#endif
