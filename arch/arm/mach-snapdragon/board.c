// SPDX-License-Identifier: GPL-2.0+
/*
 * Common initialisation for Qualcomm Snapdragon boards.
 *
 * Copyright (c) 2024 Linaro Ltd.
 * Author: Casey Connolly <casey.connolly@linaro.org>
 */

#define LOG_CATEGORY LOGC_BOARD
#define pr_fmt(fmt) "QCOM: " fmt

#include <asm/armv8/mmu.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/psci.h>
#include <asm/system.h>
#include <dm/device.h>
#include <dm/pinctrl.h>
#include <dm/uclass-internal.h>
#include <dm/read.h>
#include <power/regulator.h>
#include <env.h>
#include <fdt_support.h>
#include <init.h>
#include <linux/arm-smccc.h>
#include <linux/bug.h>
#include <linux/psci.h>
#include <linux/sizes.h>
#include <lmb.h>
#include <malloc.h>

void qcom_ramoops_early_log(const char *msg);
#include <fdt_support.h>
#include <usb.h>
#include <sort.h>
#include <time.h>

#include "qcom-priv.h"

DECLARE_GLOBAL_DATA_PTR;

enum qcom_boot_source qcom_boot_source __section(".data") = 0;

static struct mm_region rbx_mem_map[CONFIG_NR_DRAM_BANKS + 2] = { { 0 } };

struct mm_region *mem_map = rbx_mem_map;

static struct {
	phys_addr_t start;
	phys_size_t size;
} prevbl_ddr_banks[CONFIG_NR_DRAM_BANKS] __section(".data") = { 0 };

int dram_init(void)
{
	/*
	 * gd->ram_base / ram_size have been setup already
	 * in qcom_parse_memory().
	 */
	return 0;
}

static int ddr_bank_cmp(const void *v1, const void *v2)
{
	const struct {
		phys_addr_t start;
		phys_size_t size;
	} *res1 = v1, *res2 = v2;

	if (!res1->size)
		return 1;
	if (!res2->size)
		return -1;

	return (res1->start >> 24) - (res2->start >> 24);
}

/* This has to be done post-relocation since gd->bd isn't preserved */
static void qcom_configure_bi_dram(void)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		gd->bd->bi_dram[i].start = prevbl_ddr_banks[i].start;
		gd->bd->bi_dram[i].size = prevbl_ddr_banks[i].size;
	}
}

int dram_init_banksize(void)
{
	qcom_configure_bi_dram();

	return 0;
}

/**
 * The generic memory parsing code in U-Boot lacks a few things that we
 * need on Qualcomm:
 *
 * 1. It sets gd->ram_size and gd->ram_base to represent a single memory block
 * 2. setup_dest_addr() later relocates U-Boot to ram_base + ram_size, the end
 *    of that first memory block.
 *
 * This results in all memory beyond U-Boot being unusable in Linux when booting
 * with EFI.
 *
 * Since the ranges in the memory node may be out of order, the only way for us
 * to correctly determine the relocation address for U-Boot is to parse all
 * memory regions and find the highest valid address.
 *
 * We can't use fdtdec_setup_memory_banksize() since it stores the result in
 * gd->bd, which is not yet allocated.
 *
 * @fdt: FDT blob to parse /memory node from
 *
 * Return: 0 on success or -ENODATA if /memory node is missing or incomplete
 */
static int qcom_parse_memory(const void *fdt)
{
	int offset;
	const fdt64_t *memory;
	int memsize;
	phys_addr_t ram_end = 0;
	int i, j, banks;

	offset = fdt_path_offset(fdt, "/memory");
	if (offset < 0)
		return -ENODATA;

	memory = fdt_getprop(fdt, offset, "reg", &memsize);
	if (!memory)
		return -ENODATA;

	banks = min(memsize / (2 * sizeof(u64)), (ulong)CONFIG_NR_DRAM_BANKS);

	if (memsize / sizeof(u64) > CONFIG_NR_DRAM_BANKS * 2)
		log_err("Provided more than the max of %d memory banks\n", CONFIG_NR_DRAM_BANKS);

	if (banks > CONFIG_NR_DRAM_BANKS)
		log_err("Provided more memory banks than we can handle\n");

	for (i = 0, j = 0; i < banks * 2; i += 2, j++) {
		prevbl_ddr_banks[j].start = get_unaligned_be64(&memory[i]);
		prevbl_ddr_banks[j].size = get_unaligned_be64(&memory[i + 1]);
		if (!prevbl_ddr_banks[j].size) {
			j--;
			continue;
		}
		ram_end = max(ram_end, prevbl_ddr_banks[j].start + prevbl_ddr_banks[j].size);
	}

	if (!banks || !prevbl_ddr_banks[0].size)
		return -ENODATA;

	/* Sort our RAM banks -_- */
	qsort(prevbl_ddr_banks, banks, sizeof(prevbl_ddr_banks[0]), ddr_bank_cmp);

	gd->ram_base = prevbl_ddr_banks[0].start;
	gd->ram_size = ram_end - gd->ram_base;

	return 0;
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	if (gd->ram_top > 0xBFE00000ULL)
		return 0xBFE00000ULL;

	return gd->ram_top;
}

static void show_psci_version(void)
{
	struct arm_smccc_res res;

	if (fdt_node_check_compatible(gd->fdt_blob, 0, "qcom,msm8953") == 0 ||
	    fdt_node_check_compatible(gd->fdt_blob, 0, "qcom,sdm632") == 0)
		return;

	arm_smccc_smc(ARM_PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);

	/* Some older SoCs like MSM8916 don't always support PSCI */
	if ((int)res.a0 == PSCI_RET_NOT_SUPPORTED)
		return;

	debug("PSCI:  v%ld.%ld\n",
	      PSCI_VERSION_MAJOR(res.a0),
	      PSCI_VERSION_MINOR(res.a0));
}

/**
 * Most MSM8916 devices in the wild shipped without PSCI support, but the
 * upstream DTs pretend that PSCI exists. If that situation is detected here,
 * the /psci node is deleted. This is done very early to ensure the PSCI
 * firmware driver doesn't bind (which then binds a sysreset driver that won't
 * work).
 */
static void qcom_psci_fixup(void *fdt)
{
	int offset, ret;
	struct arm_smccc_res res;

	qcom_ramoops_early_log("[1.1] In qcom_psci_fixup\n");

	if (fdt_node_check_compatible(fdt, 0, "qcom,msm8953") == 0 ||
	    fdt_node_check_compatible(fdt, 0, "qcom,sdm632") == 0) {
		qcom_ramoops_early_log("[1.2] Skipping PSCI SMC on msm8953/sdm632\n");
		offset = fdt_path_offset(fdt, "/psci");
		if (offset >= 0) {
			qcom_ramoops_early_log("[1.3] Deleting /psci node\n");
			fdt_del_node(fdt, offset);
		}
		return;
	}

	arm_smccc_smc(ARM_PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);

	if ((int)res.a0 != PSCI_RET_NOT_SUPPORTED)
		return;

	offset = fdt_path_offset(fdt, "/psci");
	if (offset < 0)
		return;

	debug("Found /psci DT node on device with no PSCI. Deleting.\n");
	ret = fdt_del_node(fdt, offset);
	if (ret)
		log_err("Failed to delete /psci node: %d\n", ret);
}

#include <generated/timestamp_autogenerated.h>

static inline uint32_t get_boot_uptime_ms(void)
{
	uint64_t ticks = 0, frq = 0;
	asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	if (!frq)
		frq = 19200000;
	return (uint32_t)((ticks * 1000ULL) / frq);
}

static inline void append_ts_str(volatile char *data, volatile char *raw, uint32_t *pofft, uint32_t ms)
{
	char buf[16];
	uint32_t sec = ms / 1000;
	uint32_t rem = ms % 1000;
	int idx = 0;
	buf[idx++] = '[';
	/* Format: [12.345s] */
	if (sec >= 100) { buf[idx++] = '0' + ((sec / 100) % 10); }
	if (sec >= 10)  { buf[idx++] = '0' + ((sec / 10) % 10); }
	buf[idx++] = '0' + (sec % 10);
	buf[idx++] = '.';
	buf[idx++] = '0' + ((rem / 100) % 10);
	buf[idx++] = '0' + ((rem / 10) % 10);
	buf[idx++] = '0' + (rem % 10);
	buf[idx++] = 's';
	buf[idx++] = ']';
	buf[idx++] = ' ';
	for (int i = 0; i < idx; i++) {
		if (*pofft < 0x3FFF0) {
			data[*pofft] = buf[i];
			raw[*pofft] = buf[i];
			(*pofft)++;
		}
	}
}

/* SPMI PMIC Arbiter V2 Definitions for MSM8953 (PM8953 / PMI8952 / PMI632) */
#define SPMI_BASE                     0x02000000ULL
#define SPMI_CORE_BASE                0x0200F000ULL
#define SPMI_CHNLS_BASE               0x02400000ULL
#define SPMI_OBSRVR_BASE              0x02C00000ULL

#define SPMI_APID_MAP(n)              (SPMI_CORE_BASE + 0x800 + (4 * (n)))
#define SPMI_CH_OFFSET(n)             ((n) * 0x8000)

#define SPMI_REG_CMD0                 0x00
#define SPMI_REG_CONFIG               0x04
#define SPMI_REG_STATUS               0x08
#define SPMI_REG_WDATA                0x10
#define SPMI_REG_RDATA                0x18

#define SPMI_STATUS_DONE              0x01

/* Cached SPMI PON channels for fast petting */
static int spmi_pon_channels[4];
static int spmi_pon_channels_count = -1;

static int spmi_write_byte(int channel, uint8_t off, uint8_t val)
{
	uint64_t ch_base = SPMI_CHNLS_BASE + SPMI_CH_OFFSET(channel);
	uint32_t cmd = ((uint32_t)off) << 4; /* EXT_REG_WRITE_LONG bytecnt=0 */
	int timeout = 10000;

	*(volatile uint32_t *)(ch_base + SPMI_REG_CONFIG) = 0;
	*(volatile uint32_t *)(ch_base + SPMI_REG_WDATA) = val;
	*(volatile uint32_t *)(ch_base + SPMI_REG_CMD0) = cmd;
	asm volatile("dsb sy" ::: "memory");

	while (!(*(volatile uint32_t *)(ch_base + SPMI_REG_STATUS) & SPMI_STATUS_DONE) && --timeout);
	return (timeout > 0) ? 0 : -1;
}

static int spmi_read_byte(int channel, uint8_t off)
{
	uint64_t obs_base = SPMI_OBSRVR_BASE + SPMI_CH_OFFSET(channel);
	uint32_t cmd = (1U << 27) | (((uint32_t)off) << 4); /* EXT_REG_READ_LONG bytecnt=0 */
	int timeout = 10000;

	*(volatile uint32_t *)(obs_base + SPMI_REG_CONFIG) = 0;
	*(volatile uint32_t *)(obs_base + SPMI_REG_CMD0) = cmd;
	asm volatile("dsb sy" ::: "memory");

	while (!(*(volatile uint32_t *)(obs_base + SPMI_REG_STATUS) & SPMI_STATUS_DONE) && --timeout);
	if (timeout <= 0)
		return -1;
	return *(volatile uint32_t *)(obs_base + SPMI_REG_RDATA) & 0xFF;
}

static void spmi_pon_init(void)
{
	if (spmi_pon_channels_count >= 0)
		return;

	spmi_pon_channels_count = 0;
	for (int i = 0; i < 128; i++) {
		uint32_t val = *(volatile uint32_t *)SPMI_APID_MAP(i);
		uint32_t sid = (val >> 16) & 0xF;
		uint32_t pid = (val >> 8) & 0xFF;
		/* PON peripheral is PID 0x08 on real PMIC slaves ONLY (PM8953=sid 0, PMI8952=sid 2) */
		if (pid == 0x08 && (sid == 0 || sid == 2)) {
			if (spmi_pon_channels_count < 4) {
				spmi_pon_channels[spmi_pon_channels_count++] = i;
			}
		}
	}
}

void qcom_pmic_watchdog_pet(void)
{
	if (spmi_pon_channels_count < 0)
		spmi_pon_init();

	for (int i = 0; i < spmi_pon_channels_count; i++) {
		int ch = spmi_pon_channels[i];
		/* Write 0x80 to PON_PMIC_WD_RESET_PET (0x58) */
		spmi_write_byte(ch, 0x58, 0x80);
	}
}

void qcom_pmic_watchdog_disable(void)
{
	spmi_pon_init();

	char buf[128];
	for (int i = 0; i < spmi_pon_channels_count; i++) {
		int ch = spmi_pon_channels[i];
		uint32_t apid = *(volatile uint32_t *)SPMI_APID_MAP(ch);
		uint32_t sid = (apid >> 16) & 0xF;

		int s1_timer = spmi_read_byte(ch, 0x54);
		int s2_timer = spmi_read_byte(ch, 0x55);
		int s2_ctl   = spmi_read_byte(ch, 0x56);
		int s2_ctl2  = spmi_read_byte(ch, 0x57);

		snprintf(buf, sizeof(buf),
			 "[PMIC WD CH%d SID%d BEFORE] S1_TMR=0x%02x S2_TMR=0x%02x S2_CTL=0x%02x S2_CTL2=0x%02x\n",
			 ch, sid, s1_timer, s2_timer, s2_ctl, s2_ctl2);
		qcom_ramoops_early_log(buf);

		/* Disable PMIC Watchdog: write 0x00 to PMIC_WD_RESET_S2_CTL2 (0x57) */
		spmi_write_byte(ch, 0x57, 0x00);
		/* Pet PMIC Watchdog: write 0x80 to PON_PMIC_WD_RESET_PET (0x58) */
		spmi_write_byte(ch, 0x58, 0x80);

		int after_ctl2 = spmi_read_byte(ch, 0x57);
		snprintf(buf, sizeof(buf),
			 "[PMIC WD CH%d SID%d AFTER]  S2_CTL2=0x%02x (Disabled PMIC WDOG)\n",
			 ch, sid, after_ctl2);
		qcom_ramoops_early_log(buf);
	}
}

/* We support booting U-Boot with an internal DT when running as a first-stage bootloader
 * or for supporting quirky devices where it's easier to leave the downstream DT in place
 * to improve ABL compatibility. Otherwise, we use the DT provided by ABL.
 */
void qcom_watchdog_pet(void)
{
	struct arm_smccc_res _res;

	/* 1. APCS TMR Layout (msm8937 / msm8953 / qcs404):
	 * WDT_RST (0x38) = 1 (reload countdown counter)
	 * WDT_EN  (0x40) = 0 (keep disabled)
	 */
	arm_smccc_smc(0x82000502, 2, 0x0b017038, 1, 0, 0, 0, 0, &_res);
	arm_smccc_smc(0x82000502, 2, 0x0b017040, 0, 0, 0, 0, 0, &_res);
	*(volatile uint32_t *)0x0b017038ULL = 1;
	*(volatile uint32_t *)0x0b017040ULL = 0;

	/* 2. KPSS Layout (msm8953 / msm8952 APCS per GEMINI.md):
	 * APPS_WDOG_CTL_REG   (0x04) = 0 (keep disabled)
	 * APPS_WDOG_RESET_REG (0x08) = 1 (reload countdown counter)
	 */
	arm_smccc_smc(0x82000502, 2, 0x0b017004, 0, 0, 0, 0, 0, &_res);
	arm_smccc_smc(0x82000502, 2, 0x0b017008, 1, 0, 0, 0, 0, &_res);
	*(volatile uint32_t *)0x0b017004ULL = 0;
	*(volatile uint32_t *)0x0b017008ULL = 1;

	asm volatile("dsb sy" ::: "memory");
}

void qcom_ramoops_early_log(const char *msg)
{
	qcom_watchdog_pet();

	volatile uint32_t *pram = (volatile uint32_t *)0xBFEC0000ULL;
	volatile char *raw = (volatile char *)0xBFE80000ULL;
	volatile char *data = (volatile char *)0xBFEC000CULL;
	uint32_t offt;

	if (pram[0] != 0x43474244) {
		pram[0] = 0x43474244; /* 'DBGC' */
		pram[1] = 0;
		pram[2] = 0;
		pram[3] = 1; /* at_line_start = true */
		/* Zero out the buffers so there is no trailing garbage from DRAM */
		for (uint32_t i = 0; i < 65536; i++) {
			raw[i] = 0;
			data[i] = 0;
		}
	}

	offt = pram[1];
	uint32_t at_line_start = pram[3];

	while (*msg) {
		char c = *msg++;
		if (at_line_start && c != '\n') {
			append_ts_str(data, raw, &offt, get_boot_uptime_ms());
			at_line_start = 0;
		}
		if (offt < 0x3FFF0) {
			data[offt] = c;
			raw[offt] = c;
			offt++;
		}
		if (c == '\n') {
			at_line_start = 1;
		}
	}
	pram[1] = offt;
	pram[2] = offt;
	pram[3] = at_line_start;
	data[offt] = '\0';
	raw[offt] = '\0';
}

static inline int qcom_scm_io_write(uint32_t address, uint32_t val)
{
	struct arm_smccc_res res;
	/* SCM_SVC_IO (0x05) | SCM_IO_WRITE (0x02) -> 0x82000502, arginfo=2 */
	arm_smccc_smc(0x82000502, 2, address, val, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static inline uint32_t qcom_scm_io_read(uint32_t address)
{
	struct arm_smccc_res res;
	/* SCM_SVC_IO (0x05) | SCM_IO_READ (0x01) -> 0x82000501, arginfo=1 */
	arm_smccc_smc(0x82000501, 1, address, 0, 0, 0, 0, 0, &res);
	return (res.a0 == 0) ? (uint32_t)res.a1 : 0xFFFFFFFF;
}

static inline int qcom_scm_disable_wdog_debug(void)
{
	struct arm_smccc_res res;
	/* SCM_SVC_BOOT (0x01) | WDOG_DEBUG_DISABLE (0x09) -> 0x82000109, arginfo=2 */
	arm_smccc_smc(0x82000109, 2, 1, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

void early_delay_s(uint32_t sec)
{
	uint64_t frq, start, target;

	asm volatile("mrs %0, cntfrq_el0" : "=r" (frq));
	asm volatile("mrs %0, cntpct_el0" : "=r" (start));
	target = start + (frq * sec);
	while (1) {
		uint64_t cur;
		qcom_watchdog_pet();
		asm volatile("mrs %0, cntpct_el0" : "=r" (cur));
		if (cur >= target)
			break;
	}
}

static void qcom_disable_watchdog(void)
{
	/* 1. TrustZone SCM WDOG_DEBUG_DISABLE */
	qcom_scm_disable_wdog_debug();

	/* 2. APCS TMR Watchdog (msm8937 / msm8953 / qcs404):
	 * WDT_RST       = 0x38 (write 1 to reload)
	 * WDT_EN        = 0x40 (write 0 to disable)
	 * WDT_BARK_TIME = 0x4C (program maximum timeout)
	 * WDT_BITE_TIME = 0x5C (program maximum timeout)
	 */
	qcom_scm_io_write(0x0b017040, 0);
	qcom_scm_io_write(0x0b017038, 1);
	qcom_scm_io_write(0x0b01704c, 0x000FFFFF);
	qcom_scm_io_write(0x0b01705c, 0x000FFFFF);

	*(volatile uint32_t *)0x0b01704cULL = 0x000FFFFF;
	*(volatile uint32_t *)0x0b01705cULL = 0x000FFFFF;
	*(volatile uint32_t *)0x0b017038ULL = 1;
	*(volatile uint32_t *)0x0b017040ULL = 0;

	/* 3. KPSS Watchdog (MSM8953 / MSM8952 APCS per GEMINI.md):
	 * APPS_WDOG_CTL_REG   (0x04) = 0 (disable)
	 * APPS_WDOG_RESET_REG (0x08) = 1 (reload counter)
	 * APPS_WDOG_BARK_VAL  (0x10) = 0x7FFFFFFF
	 * APPS_WDOG_BITE_VAL  (0x14) = 0x7FFFFFFF
	 */
	qcom_scm_io_write(0x0b017004, 0);
	qcom_scm_io_write(0x0b017008, 1);
	qcom_scm_io_write(0x0b017010, 0x7FFFFFFF);
	qcom_scm_io_write(0x0b017014, 0x7FFFFFFF);

	*(volatile uint32_t *)0x0b017010ULL = 0x7FFFFFFF;
	*(volatile uint32_t *)0x0b017014ULL = 0x7FFFFFFF;
	*(volatile uint32_t *)0x0b017004ULL = 0;
	*(volatile uint32_t *)0x0b017008ULL = 1;
	asm volatile("dsb sy" ::: "memory");

	/* 4. SPMI PMIC Watchdog Disarming (500ms S2 Timer) */
	qcom_pmic_watchdog_disable();
}

static void log_boot_timestamp(void)
{
	uint64_t ticks = 0, frq = 0;
	char buf[256];

	asm volatile("mrs %0, cntpct_el0" : "=r" (ticks));
	asm volatile("mrs %0, cntfrq_el0" : "=r" (frq));

	if (!frq)
		frq = 19200000;

	uint32_t sec = (uint32_t)(ticks / frq);
	uint32_t rem_ms = (uint32_t)(((ticks % frq) * 1000ULL) / frq);

	snprintf(buf, sizeof(buf),
		 "[BOOT TIME] Hey! I started at T+%u.%03u seconds after SoC power-on (Ticks: %llu @ %llu Hz)\n",
		 sec, rem_ms, (unsigned long long)ticks, (unsigned long long)frq);
	qcom_ramoops_early_log(buf);

	snprintf(buf, sizeof(buf),
		 "[BUILD] U-Boot compiled on " U_BOOT_DATE " at " U_BOOT_TIME " UTC\n");
	qcom_ramoops_early_log(buf);

	/* Read hardware APCS TMR watchdog registers BEFORE */
	uint32_t b_tmr_rst  = *(volatile uint32_t *)0x0b017038ULL;
	uint32_t b_tmr_en   = *(volatile uint32_t *)0x0b017040ULL;
	uint32_t b_tmr_sts  = *(volatile uint32_t *)0x0b017044ULL;
	uint32_t b_tmr_bark = *(volatile uint32_t *)0x0b01704cULL;
	uint32_t b_tmr_bite = *(volatile uint32_t *)0x0b01705cULL;

	uint32_t scm_b_tmr_en   = qcom_scm_io_read(0x0b017040);
	uint32_t scm_b_tmr_sts  = qcom_scm_io_read(0x0b017044);
	uint32_t scm_b_tmr_bite = qcom_scm_io_read(0x0b01705c);

	/* Read legacy KPSS watchdog registers BEFORE */
	uint32_t b_ctl  = *(volatile uint32_t *)0x0b017004ULL;
	uint32_t b_rst  = *(volatile uint32_t *)0x0b017008ULL;
	uint32_t b_sts  = *(volatile uint32_t *)0x0b01700cULL;
	uint32_t b_bark = *(volatile uint32_t *)0x0b017010ULL;
	uint32_t b_bite = *(volatile uint32_t *)0x0b017014ULL;

	/* Reprogram & pet via SCM and MMIO */
	int sdi_ret = qcom_scm_disable_wdog_debug();
	qcom_disable_watchdog();

	/* Read hardware APCS TMR watchdog registers AFTER */
	uint32_t a_tmr_rst  = *(volatile uint32_t *)0x0b017038ULL;
	uint32_t a_tmr_en   = *(volatile uint32_t *)0x0b017040ULL;
	uint32_t a_tmr_sts  = *(volatile uint32_t *)0x0b017044ULL;
	uint32_t a_tmr_bark = *(volatile uint32_t *)0x0b01704cULL;
	uint32_t a_tmr_bite = *(volatile uint32_t *)0x0b01705cULL;

	uint32_t scm_a_tmr_en   = qcom_scm_io_read(0x0b017040);
	uint32_t scm_a_tmr_sts  = qcom_scm_io_read(0x0b017044);
	uint32_t scm_a_tmr_bite = qcom_scm_io_read(0x0b01705c);

	/* Read legacy KPSS watchdog registers AFTER */
	uint32_t a_ctl  = *(volatile uint32_t *)0x0b017004ULL;
	uint32_t a_rst  = *(volatile uint32_t *)0x0b017008ULL;
	uint32_t a_sts  = *(volatile uint32_t *)0x0b01700cULL;
	uint32_t a_bark = *(volatile uint32_t *)0x0b017010ULL;
	uint32_t a_bite = *(volatile uint32_t *)0x0b017014ULL;

	snprintf(buf, sizeof(buf),
		 "[APCS TMR BEFORE] MMIO: RST(0x38)=0x%08x EN(0x40)=0x%08x STS(0x44)=0x%08x BARK=0x%08x BITE=0x%08x | SCM: EN=0x%08x STS=0x%08x BITE=0x%08x\n",
		 b_tmr_rst, b_tmr_en, b_tmr_sts, b_tmr_bark, b_tmr_bite, scm_b_tmr_en, scm_b_tmr_sts, scm_b_tmr_bite);
	qcom_ramoops_early_log(buf);

	snprintf(buf, sizeof(buf),
		 "[APCS TMR AFTER]  MMIO: RST(0x38)=0x%08x EN(0x40)=0x%08x STS(0x44)=0x%08x BARK=0x%08x BITE=0x%08x | SCM: EN=0x%08x STS=0x%08x BITE=0x%08x (SDI_RET=%d)\n",
		 a_tmr_rst, a_tmr_en, a_tmr_sts, a_tmr_bark, a_tmr_bite, scm_a_tmr_en, scm_a_tmr_sts, scm_a_tmr_bite, sdi_ret);
	qcom_ramoops_early_log(buf);

	snprintf(buf, sizeof(buf),
		 "[KPSS BEFORE]     MMIO: CTL(0x04)=0x%08x RST(0x08)=0x%08x STS(0x0C)=0x%08x BARK(0x10)=0x%08x BITE(0x14)=0x%08x\n",
		 b_ctl, b_rst, b_sts, b_bark, b_bite);
	qcom_ramoops_early_log(buf);

	snprintf(buf, sizeof(buf),
		 "[KPSS AFTER]      MMIO: CTL(0x04)=0x%08x RST(0x08)=0x%08x STS(0x0C)=0x%08x BARK(0x10)=0x%08x BITE(0x14)=0x%08x\n",
		 a_ctl, a_rst, a_sts, a_bark, a_bite);
	qcom_ramoops_early_log(buf);
}

static void draw_early_splash(void)
{
	/* Continuous splash on onclite is 24bpp RGB888 at 0x90001000 */
	volatile uint8_t *fb = (volatile uint8_t *)0x90001000ULL;
	int y, x;

	for (y = 0; y < 1520; y++) {
		uint8_t r, g, b;
		if (y < 250) {
			r = 0xFF; g = 0x00; b = 0x00; /* Red */
		} else if (y < 500) {
			r = 0x00; g = 0xFF; b = 0x00; /* Green */
		} else if (y < 750) {
			r = 0x00; g = 0x00; b = 0xFF; /* Blue */
		} else if (y < 1000) {
			r = 0x00; g = 0xFF; b = 0xFF; /* Cyan */
		} else if (y < 1250) {
			r = 0xFF; g = 0xFF; b = 0x00; /* Yellow */
		} else {
			r = 0xFF; g = 0x00; b = 0xFF; /* Magenta */
		}

		for (x = 0; x < 720; x++) {
			int idx = (y * 720 + x) * 3;
			fb[idx + 0] = b;
			fb[idx + 1] = g;
			fb[idx + 2] = r;
		}
	}
}

int board_fdt_blob_setup(void **fdtp)
{
	struct fdt_header *external_fdt, *internal_fdt;
	bool internal_valid, external_valid;
	int ret = -ENODATA;

	qcom_disable_watchdog();
	draw_early_splash();
	qcom_ramoops_early_log("=== U-BOOT MSM8953 / ONCLITE EARLY BOOT (v40) ===\n[0] In board_fdt_blob_setup\n");
	log_boot_timestamp();
	qcom_ramoops_early_log("[SPLASH] Painted early 24bpp RGB888 test bars on LCD display!\n");
	qcom_ramoops_early_log("[CKPT 0] Splash painted OK\n");

	internal_fdt = (struct fdt_header *)*fdtp;
	external_fdt = (struct fdt_header *)get_prev_bl_fdt_addr();
	external_valid = external_fdt && !fdt_check_header(external_fdt);
	internal_valid = !fdt_check_header(internal_fdt);

	/*
	 * There is no point returning an error here, U-Boot can't do anything useful in this situation.
	 * Bail out while we can still print a useful error message.
	 */
	if (!internal_valid && !external_valid) {
		qcom_ramoops_early_log("FATAL: Neither internal nor external FDT is valid!\n");
		panic("Internal FDT is invalid and no external FDT was provided! (fdt=%#llx)\n",
		      (phys_addr_t)external_fdt);
	}

	/* Prefer memory information from internal DT if it's present */
	if (internal_valid)
		ret = qcom_parse_memory(internal_fdt);

	if (ret < 0 && external_valid) {
		/* No internal FDT or it lacks a proper /memory node.
		 * The previous bootloader handed us something, let's try that.
		 */
		if (internal_valid)
			debug("No memory info in internal FDT, falling back to external\n");

		ret = qcom_parse_memory(external_fdt);
	}

	if (ret < 0) {
		qcom_ramoops_early_log("FATAL: No valid memory ranges found!\n");
		panic("No valid memory ranges found!\n");
	}

	qcom_ramoops_early_log("[1] Memory ranges parsed OK\n");

	/* If we have an external FDT, it can only have come from the Android bootloader. */
	if (external_valid)
		qcom_boot_source = QCOM_BOOT_SOURCE_ANDROID;
	else
		qcom_boot_source = QCOM_BOOT_SOURCE_XBL;

	debug("ram_base = %#011lx, ram_size = %#011llx\n",
	      gd->ram_base, gd->ram_size);

	if (internal_valid) {
		debug("Using built in FDT\n");
		ret = -EEXIST;
	} else {
		debug("Using external FDT\n");
		*fdtp = external_fdt;
		ret = 0;
	}

	qcom_psci_fixup(*fdtp);
	qcom_ramoops_early_log("[1.4] Exiting board_fdt_blob_setup\n");

	return ret;
}

/*
 * Some Qualcomm boards require GPIO configuration when switching USB modes.
 * Support setting this configuration via pinctrl state.
 */
int board_usb_init(int index, enum usb_init_type init)
{
	struct udevice *usb;
	int ret = 0;

	/* USB device */
	ret = uclass_find_device_by_seq(UCLASS_USB, index, &usb);
	if (ret) {
		printf("Cannot find USB device\n");
		return ret;
	}

	ret = dev_read_stringlist_search(usb, "pinctrl-names",
					 "device");
	/* No "device" pinctrl state, so just bail */
	if (ret < 0)
		return 0;

	/* Select "default" or "device" pinctrl */
	switch (init) {
	case USB_INIT_HOST:
		pinctrl_select_state(usb, "default");
		break;
	case USB_INIT_DEVICE:
		pinctrl_select_state(usb, "device");
		break;
	default:
		debug("Unknown usb_init_type %d\n", init);
		break;
	}

	return 0;
}

/*
 * Some boards still need board specific init code, they can implement that by
 * overriding this function.
 *
 * FIXME: get rid of board specific init code
 */
void __weak qcom_board_init(void)
{
}

int board_init(void)
{
	qcom_disable_watchdog();
	qcom_ramoops_early_log("[3] Entering board_init\n");
	show_psci_version();
	qcom_board_init();
	qcom_ramoops_early_log("[3.9] Exiting board_init\n");
	return 0;
}

/**
 * out_len includes the trailing null space
 */
static int get_cmdline_option(const char *cmdline, const char *key, char *out, int out_len)
{
	const char *p, *p_end;
	int len;

	p = strstr(cmdline, key);
	if (!p)
		return -ENOENT;

	p += strlen(key);
	p_end = strstr(p, " ");
	if (!p_end)
		return -ENOENT;

	len = p_end - p;
	if (len > out_len)
		len = out_len;

	strncpy(out, p, len);
	out[len] = '\0';

	return 0;
}

/* The bootargs are populated by the previous stage bootloader */
static const char *get_cmdline(void)
{
	ofnode node;
	static const char *cmdline = NULL;

	if (cmdline)
		return cmdline;

	node = ofnode_path("/chosen");
	if (!ofnode_valid(node))
		return NULL;

	cmdline = ofnode_read_string(node, "bootargs");

	return cmdline;
}

void qcom_set_serialno(void)
{
	const char *cmdline = get_cmdline();
	char serial[32];

	if (!cmdline) {
		log_debug("Failed to get bootargs\n");
		return;
	}

	get_cmdline_option(cmdline, "androidboot.serialno=", serial, sizeof(serial));
	if (serial[0] != '\0')
		env_set("serial#", serial);
}

/* Sets up the "board", and "soc" environment variables as well as constructing the devicetree
 * path, with a few quirks to handle non-standard dtb filenames. This is not meant to be a
 * comprehensive solution to automatically picking the DTB, but aims to be correct for the
 * majority case. For most devices it should be possible to make this algorithm work by
 * adjusting the root compatible property in the U-Boot DTS. Handling devices with multiple
 * variants that are all supported by a single U-Boot image will require implementing device-
 * specific detection.
 */
static void configure_env(void)
{
	const char *first_compat, *last_compat;
	char *tmp;
	char buf[32] = { 0 };
	/*
	 * Most DTB filenames follow the scheme: qcom/<soc>-[vendor]-<board>.dtb
	 * The vendor is skipped when it's a Qualcomm reference board, or the
	 * db845c.
	 */
	char dt_path[64] = { 0 };
	int compat_count, ret;
	ofnode root;

	root = ofnode_root();
	/* This is almost always 2, but be explicit that we want the first and last compatibles
	 * not the first and second.
	 */
	compat_count = ofnode_read_string_count(root, "compatible");
	if (compat_count < 2) {
		log_warning("%s: only one root compatible bailing!\n", __func__);
		return;
	}

	/* The most specific device compatible (e.g. "thundercomm,db845c") */
	ret = ofnode_read_string_index(root, "compatible", 0, &first_compat);
	if (ret < 0) {
		log_warning("Can't read first compatible\n");
		return;
	}

	strlcpy(buf, first_compat, sizeof(buf) - 1);
	tmp = buf;

	/* The Qualcomm reference boards (RBx, HDK, etc)  */
	if (!strncmp("qcom", buf, strlen("qcom"))) {
		char *soc;

		/*
		 * They all have the first compatible as "qcom,<soc>-<board>"
		 * (e.g. "qcom,qrb5165-rb5"). We extract just the part after
		 * the dash.
		 */
		if (!strsep(&tmp, ",")) {
			log_warning("compatible '%s' has no ','\n", buf);
			return;
		}
		soc = strsep(&tmp, "-");
		if (!soc) {
			log_warning("compatible '%s' has no '-'\n", buf);
			return;
		}

		env_set("soc", soc);
		env_set("board", tmp);
	} else {
		if (!strsep(&tmp, ",")) {
			log_warning("compatible '%s' has no ','\n", buf);
			return;
		}
		/*
		 * For thundercomm we just want the bit after the comma
		 * (e.g. "db845c"), for all other boards we replace the comma
		 * with a '-' and take both (e.g. "oneplus-enchilada")
		 */
		if (!strncmp("thundercomm", buf, strlen("thundercomm"))) {
			env_set("board", tmp);
		} else {
			*(tmp - 1) = '-';
			env_set("board", buf);
		}

		/* The last compatible is always the SoC compatible */
		ret = ofnode_read_string_index(root, "compatible",
					       compat_count - 1, &last_compat);
		if (ret < 0) {
			log_warning("Can't read second compatible\n");
			return;
		}

		/* Copy the last compat (e.g. "qcom,sdm845") into buf */
		memset(buf, 0, sizeof(buf));
		strlcpy(buf, last_compat, sizeof(buf) - 1);
		tmp = buf;

		/* strsep() is destructive, it replaces the comma with a \0 */
		if (!strsep(&tmp, ",")) {
			log_warning("second compatible '%s' has no ','\n", buf);
			return;
		}

		/* tmp now points to just the "sdm845" part of the string */
		env_set("soc", tmp);
	}

	/* Now build the full path name */
	snprintf(dt_path, sizeof(dt_path), "qcom/%s-%s.dtb",
		 env_get("soc"), env_get("board"));
	env_set("fdtfile", dt_path);

	qcom_set_serialno();
}

void qcom_show_boot_source(void)
{
	const char *name = "UNKNOWN";

	switch (qcom_boot_source) {
	case QCOM_BOOT_SOURCE_ANDROID:
		name = "ABL";
		break;
	case QCOM_BOOT_SOURCE_XBL:
		name = "XBL";
		break;
	}

	log_info("U-Boot loaded from %s\n", name);
	env_set("boot_source", name);
}

void __weak qcom_late_init(void)
{
}

#define KERNEL_COMP_SIZE	SZ_64M
#ifdef CONFIG_FASTBOOT_BUF_SIZE
#define FASTBOOT_BUF_SIZE CONFIG_FASTBOOT_BUF_SIZE
#else
#define FASTBOOT_BUF_SIZE 0
#endif

#define lmb_alloc(size, addr) lmb_alloc_mem(LMB_MEM_ALLOC_ANY, SZ_2M, addr, size, LMB_NONE)

/* Stolen from arch/arm/mach-apple/board.c */
int board_late_init(void)
{
	u32 status = 0, fdt_status = 0;
	phys_addr_t addr;
	struct fdt_header *fdt_blob = (struct fdt_header *)gd->fdt_blob;

	/* We need to be fairly conservative here as we support boards with just 1G of TOTAL RAM */
	status |= !lmb_alloc(SZ_128M, &addr) ?
		env_set_hex("loadaddr", addr) : 1;
	status |= env_set_hex("kernel_addr_r", addr);
	status |= !lmb_alloc(SZ_128M, &addr) ?
		env_set_hex("ramdisk_addr_r", addr) : 1;
	status |= !lmb_alloc(KERNEL_COMP_SIZE, &addr) ?
		env_set_hex("kernel_comp_addr_r", addr) : 1;
	status |= env_set_hex("kernel_comp_size", KERNEL_COMP_SIZE);
	status |= !lmb_alloc(SZ_4M, &addr) ?
		env_set_hex("scriptaddr", addr) : 1;
	status |= !lmb_alloc(SZ_4M, &addr) ?
		env_set_hex("pxefile_addr_r", addr) : 1;

	if (IS_ENABLED(CONFIG_FASTBOOT)) {
		status |= !lmb_alloc(FASTBOOT_BUF_SIZE, &addr) ?
			env_set_hex("fastboot_addr_r", addr) : 1;
		/*
		 * Override loadaddr for memory rich soc since ${loadaddr} and
		 * ${kernel_addr_r} need to be different for the Android boot image
		 * flow. It's typically safe for ${loadaddr} to be the same address
		 * as the fastboot buffer.
		 */
		status |= env_set_hex("loadaddr", addr);
	}

	fdt_status |= !lmb_alloc(SZ_2M, &addr) ?
		env_set_hex("fdt_addr_r", addr) : 1;

	if (IS_ENABLED(CONFIG_OF_LIBFDT_OVERLAY)) {
		status |= !lmb_alloc(SZ_1M, &addr) ?
			env_set_hex("fdtoverlay_addr_r", addr) : 1;
	}

	if (status || fdt_status)
		log_warning("%s: Failed to set run time variables\n", __func__);

	/* By default copy U-Boots FDT, it will be used as a fallback */
	if (fdt_status)
		log_warning("%s: Failed to reserve memory for copying FDT\n",
			    __func__);
	else
		memcpy((void *)addr, (void *)gd->fdt_blob,
		       fdt32_to_cpu(fdt_blob->totalsize));

	qcom_ramoops_early_log("[BLATE 1] configure_env start\n");
	configure_env();
	qcom_ramoops_early_log("[BLATE 2] configure_env done, calling qcom_late_init\n");
	qcom_late_init();
	qcom_ramoops_early_log("[BLATE 3] qcom_late_init done, calling qcom_show_boot_source\n");
	qcom_show_boot_source();
	qcom_ramoops_early_log("[BLATE 4] qcom_show_boot_source done\n");
	/* Configure the dfu_string for capsule updates (skip on msm8953) */
	if (fdt_node_check_compatible(gd->fdt_blob, 0, "qcom,msm8953") != 0 &&
	    fdt_node_check_compatible(gd->fdt_blob, 0, "qcom,sdm632") != 0)
		qcom_configure_capsule_updates();

	qcom_ramoops_early_log("[BLATE 5] board_late_init finished OK!\n");
	return 0;
}

static void build_mem_map(void)
{
	int i, j;

	qcom_configure_bi_dram();

	/*
	 * Ensure the peripheral block is sized to correctly cover the address range
	 * up to the first memory bank.
	 * Don't map the first page to ensure that we actually trigger an abort on a
	 * null pointer access rather than just hanging.
	 * FIXME: we should probably split this into more precise regions
	 */
	mem_map[0].phys = 0x1000;
	mem_map[0].virt = mem_map[0].phys;
	mem_map[0].size = gd->bd->bi_dram[0].start - mem_map[0].phys;
	mem_map[0].attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN;

	for (i = 1, j = 0; i < ARRAY_SIZE(rbx_mem_map) - 2 && gd->bd->bi_dram[j].size; i++, j++) {
		mem_map[i].phys = gd->bd->bi_dram[j].start;
		mem_map[i].virt = mem_map[i].phys;
		mem_map[i].size = gd->bd->bi_dram[j].size;
		mem_map[i].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | \
				   PTE_BLOCK_INNER_SHARE;
	}

	/* Map Ramoops region (0xBFE00000 - 0xC0000000) so early logging works with MMU enabled */
	mem_map[i].phys = 0xBFE00000ULL;
	mem_map[i].virt = mem_map[i].phys;
	mem_map[i].size = 0x00200000ULL; /* 2 MiB */
	mem_map[i].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | \
			   PTE_BLOCK_INNER_SHARE;
	i++;

	mem_map[i].phys = UINT64_MAX;
	mem_map[i].virt = 0;
	mem_map[i].size = 0;
	mem_map[i].attrs = 0;

#ifdef DEBUG
	debug("Configured memory map:\n");
	for (i = 0; mem_map[i].size; i++)
		debug("  0x%016llx - 0x%016llx: entry %d\n",
		      mem_map[i].phys, mem_map[i].phys + mem_map[i].size, i);
#endif
}

u64 get_page_table_size(void)
{
	return SZ_1M;
}

static int fdt_cmp_res(const void *v1, const void *v2)
{
	const struct fdt_resource *res1 = v1, *res2 = v2;

	return res1->start - res2->start;
}

#define N_RESERVED_REGIONS 32

/* Mark all no-map regions as PTE_TYPE_FAULT to prevent speculative access.
 * On some platforms this is enough to trigger a security violation and trap
 * to EL3.
 */
static void carve_out_reserved_memory(void)
{
	static struct fdt_resource res[N_RESERVED_REGIONS] = { 0 };
	int parent, rmem, count, i = 0;
	phys_addr_t start;
	size_t size;

	/* Some reserved nodes must be carved out, as the cache-prefetcher may otherwise
	 * attempt to access them, causing a security exception.
	 */
	parent = fdt_path_offset(gd->fdt_blob, "/reserved-memory");
	if (parent <= 0) {
		log_err("No reserved memory regions found\n");
		return;
	}

	/* Collect the reserved memory regions */
	fdt_for_each_subnode(rmem, gd->fdt_blob, parent) {
		const fdt32_t *ptr;
		int len;
		if (!fdt_getprop(gd->fdt_blob, rmem, "no-map", NULL))
			continue;

		if (i == N_RESERVED_REGIONS) {
			log_err("Too many reserved regions!\n");
			break;
		}

		/* Read the address and size out from the reg property. Doing this "properly" with
		 * fdt_get_resource() takes ~70ms on SDM845, but open-coding the happy path here
		 * takes <1ms... Oh the woes of no dcache.
		 */
		ptr = fdt_getprop(gd->fdt_blob, rmem, "reg", &len);
		if (ptr) {
			/* Qualcomm devices use #address/size-cells = <2> but all reserved regions are within
			 * the 32-bit address space. So we can cheat here for speed.
			 */
			phys_addr_t r_start = fdt32_to_cpu(ptr[1]);
			phys_addr_t r_end = r_start + fdt32_to_cpu(ptr[3]);

			/* Skip regions outside mapped memory to prevent infinite loop in mmu_change_region_attr */
			if (r_start < gd->ram_base || r_start >= gd->ram_top || r_end > gd->ram_top)
				continue;

			res[i].start = r_start;
			res[i].end = r_end;
			i++;
		}
	}

	/* If no valid reserved memory regions to carve out, return early */
	if (i == 0)
		return;

	/* Sort the reserved memory regions by address */
	count = i;
	qsort(res, count, sizeof(struct fdt_resource), fdt_cmp_res);

	/* Now set the right attributes for them. Often a lot of the regions are tightly packed together
	 * so we can optimise the number of calls to mmu_change_region_attr() by combining adjacent
	 * regions.
	 */
	start = ALIGN_DOWN(res[0].start, SZ_2M);
	size = ALIGN(res[0].end - start, SZ_2M);
	for (i = 1; i <= count; i++) {
		/* We ideally want to 2M align everything for more efficient pagetables, but we must avoid
		 * overwriting reserved memory regions which shouldn't be mapped as FAULT (like those with
		 * compatible properties).
		 * If within 2M of the previous region, bump the size to include this region. Otherwise
		 * start a new region.
		 */
		if (i == count || start + size < res[i].start - SZ_2M) {
			debug("  0x%016llx - 0x%016llx: reserved\n",
			      start, start + size);
			mmu_change_region_attr(start, size, PTE_TYPE_FAULT);
			/* If this is the final region then quit here before we index
			 * out of bounds...
			 */
			if (i == count)
				break;
			start = ALIGN_DOWN(res[i].start, SZ_2M);
			size = ALIGN(res[i].end - start, SZ_2M);
		} else {
			/* Bump size if this region is immediately after the previous one */
			size = ALIGN(res[i].end - start, SZ_2M);
		}
	}
}

/* This function open-codes setup_all_pgtables() so that we can
 * insert additional mappings *before* turning on the MMU.
 */
void enable_caches(void)
{
	u64 tlb_addr = gd->arch.tlb_addr;
	u64 tlb_size = gd->arch.tlb_size;
	u64 pt_size;
	ulong carveout_start;

	qcom_ramoops_early_log("[2.0] Entering enable_caches\n");

	gd->arch.tlb_fillptr = tlb_addr;

	build_mem_map();
	qcom_ramoops_early_log("[2.1] build_mem_map done\n");

	icache_enable();
	qcom_ramoops_early_log("[2.2] icache_enable done\n");

	/* Create normal system page tables */
	setup_pgtables();
	qcom_ramoops_early_log("[2.3] setup_pgtables primary done\n");

	pt_size = (uintptr_t)gd->arch.tlb_fillptr -
		  (uintptr_t)gd->arch.tlb_addr;
	debug("Primary pagetable size: %lluKiB\n", pt_size / 1024);

	/* Create emergency page tables */
	gd->arch.tlb_size -= pt_size;
	gd->arch.tlb_addr = gd->arch.tlb_fillptr;
	setup_pgtables();
	gd->arch.tlb_emerg = gd->arch.tlb_addr;
	gd->arch.tlb_addr = tlb_addr;
	gd->arch.tlb_size = tlb_size;
	qcom_ramoops_early_log("[2.4] setup_pgtables emerg done\n");

	/* We do the carveouts only for QCS404, for now. */
	if (fdt_node_check_compatible(gd->fdt_blob, 0, "qcom,qcs404") == 0) {
		carveout_start = get_timer(0);
		carve_out_reserved_memory();
		debug("carveout time: %lums\n", get_timer(carveout_start));
	}

	qcom_ramoops_early_log("[2.5.1] Invalidating TLB\n");
	__asm_invalidate_tlb_all();

	qcom_ramoops_early_log("[2.5.2] Setting TTBR/TCR/MAIR\n");
	mmu_setup();

	qcom_ramoops_early_log("[2.5.3] Enabling MMU (CR_M)\n");
	mmu_enable();

	qcom_ramoops_early_log("[2.5.4] MMU ON! Invalidating dcache\n");
	invalidate_dcache_all();

	qcom_ramoops_early_log("[2.5.5] Enabling dcache (CR_C)\n");
	set_sctlr(get_sctlr() | CR_C);

	qcom_ramoops_early_log("[2.6] MMU and dcache enabled successfully!\n");
}
