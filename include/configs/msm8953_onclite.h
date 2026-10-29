/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board configuration file for Qualcomm MSM8953 / Snapdragon 632
 * Target: Xiaomi Redmi 7 (onclite / onc)
 *
 * (C) Copyright 2026 Antigravity Embedded Systems
 */

#ifndef __CONFIGS_MSM8953_ONCLITE_H
#define __CONFIGS_MSM8953_ONCLITE_H

#include <linux/sizes.h>
#include <configs/qcom.h>

/* Physical Memory Map */
#define PHYS_SDRAM_1			0x80000000
#define PHYS_SDRAM_1_SIZE		SZ_2G
#define PHYS_SDRAM_2			0x40000000
#define PHYS_SDRAM_2_SIZE		SZ_1G

#define CFG_SYS_SDRAM_BASE		PHYS_SDRAM_1

/*
 * Memory Layout for Chainloading via LK / lk2nd:
 *
 * 0x80000000 - 0x80080000: Reserved (Early page tables / LK headers)
 * 0x80080000 - 0x8F600000: U-Boot Load / Execution Region (CONFIG_TEXT_BASE)
 * 0x8F600000 - 0x90000000: lk2nd Execution Carveout (10 MiB - PRESERVED)
 * 0x90000000 - 0x90800000: Continuous Splash Framebuffer (SimpleFB - PRESERVED)
 * 0xA0000000 - 0xA1FFFFFF: CONFIG_SYS_LOAD_ADDR (Scratch Buffer)
 * 0xA2000000 - 0xB2000000: Fastboot Buffer (256 MiB)
 */
#ifndef CONFIG_SYS_LOAD_ADDR
#define CONFIG_SYS_LOAD_ADDR		0xA0000000
#endif

#ifndef CFG_SYS_BAUDRATE_TABLE
#define CFG_SYS_BAUDRATE_TABLE		{ 115200, 230400, 460800, 921600 }
#endif

/* Fastboot download buffer configuration */
#define CFG_FASTBOOT_BUF_ADDR		0xA2000000
#define CFG_FASTBOOT_BUF_SIZE		SZ_256M

#endif /* __CONFIGS_MSM8953_ONCLITE_H */
