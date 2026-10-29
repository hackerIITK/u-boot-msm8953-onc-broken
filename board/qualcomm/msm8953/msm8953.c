// SPDX-License-Identifier: GPL-2.0+
/*
 * Board initialization for Qualcomm MSM8953 / Snapdragon 632
 * Target: Xiaomi Redmi 7 (onclite / onc)
 *
 * (C) Copyright 2026 Antigravity Embedded Systems
 */

#include <button.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/pinctrl.h>
#include <env.h>
#include <init.h>
#include <mmc.h>
#include <net.h>
#include <asm/cache.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <fdt_support.h>
#include <linux/delay.h>
#include <stdio_dev.h>
#include <linux/sizes.h>

extern void qcom_ramoops_early_log(const char *msg);

static void ramoops_stdio_putc(struct stdio_dev *dev, const char c)
{
	char s[2] = { c, '\0' };
	qcom_ramoops_early_log(s);
}

static void ramoops_stdio_puts(struct stdio_dev *dev, const char *s)
{
	qcom_ramoops_early_log(s);
}

static int register_ramoops_stdio(void)
{
	struct stdio_dev dev;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "ramoops");
	dev.flags = DEV_FLAGS_OUTPUT;
	dev.putc = ramoops_stdio_putc;
	dev.puts = ramoops_stdio_puts;

	return stdio_register(&dev);
}

#define QCOM_SOC_MSM8953	293
#define QCOM_SOC_APQ8053	304
#define QCOM_SOC_SDM450		338
#define QCOM_SOC_SDM632		349
#define QCOM_SOC_SDA450		351
#define QCOM_SOC_SDA632		350

#define MSM8953_UART_BASE	0x078AF000
#define MSM8953_VOL_UP_GPIO	85

static u32 msm8953_board_serial(void)
{
	return 0x89530001;
}

static void msm8953_generate_mac_addr(u8 *mac)
{
	/* Locally administered unicast MAC */
	mac[0] = 0x02;
	mac[1] = 0x00;

	put_unaligned_be32(msm8953_board_serial(), &mac[2]);
}

void qcom_board_init(void)
{
	qcom_ramoops_early_log("Board: Xiaomi Redmi 7 (onclite / sdm632)\n");
}

int qcom_late_init(void)
{
	char serial[16];
	u8 mac[ARP_HLEN];

	register_ramoops_stdio();
	qcom_ramoops_early_log("[4] Ramoops stdio console registered\n");

	memset(serial, 0, sizeof(serial));
	snprintf(serial, sizeof(serial), "%08x", msm8953_board_serial());
	env_set("serial#", serial);
	env_set("board_name", "onclite");
	env_set("soc_name", "msm8953");

	if (!eth_env_get_enetaddr("ethaddr", mac)) {
		msm8953_generate_mac_addr(mac);
		eth_env_set_enetaddr("ethaddr", mac);
	}

	return 0;
}
