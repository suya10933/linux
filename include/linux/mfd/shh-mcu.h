/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_SHH_MCU_H
#define __LINUX_MFD_SHH_MCU_H

#include <linux/bits.h>

/* base : 0x00 - rx_buf (26 bytes) (read only)
 * 0x00-0x19	DATA (buttons, joysticks, battery, etc..)
 */

/* base : 0x20 - tx_buf (26 bytes) (write only)
 * 0x00-0x19	DATA (LED control, LCD control, etc..)
 */

#define SHH_MCU_RAM_SIZE	64
#define SHH_MCU_XFER_LENGTH	32
#define SHH_MCU_ADDR_MASK	GENMASK(4, 0)
#define SHH_MCU_DATA_SIZE	26
#define SHH_MCU_MAGIC		0xDEADBEEF

#define SHH_MCU_RX_BASE		0x00
#define SHH_MCU_RX_SIZE		26
#define SHH_MCU_TX_BASE		0x20
#define SHH_MCU_TX_SIZE		26

#endif
