// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 * (C) Copyright 2002-2006
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2002
 * Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Marius Groeger <mgroeger@sysgo.de>
 */

#include <config.h>
#include <api.h>
#include <bootstage.h>
#include <cpu_func.h>
#include <cyclic.h>
#include <display_options.h>
#include <exports.h>
#ifdef CONFIG_MTD_NOR_FLASH
#include <flash.h>
#endif
#include <hang.h>
#include <image.h>
#include <irq_func.h>
#include <lmb.h>
#include <log.h>
#include <net.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <u-boot/crc.h>
#include <binman.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <efi_loader.h>
#include <env.h>
#include <env_internal.h>
#include <fdtdec.h>
#include <init.h>
#include <initcall.h>
#include <kgdb.h>
#include <irq_func.h>
#include <led.h>
#include <malloc.h>
#include <mapmem.h>
#include <miiphy.h>
#include <mmc.h>
#include <mux.h>
#include <nand.h>
#include <of_live.h>

extern void qcom_ramoops_early_log(const char *msg);
extern void early_delay_s(uint32_t sec);
#include <onenand_uboot.h>
#include <pvblock.h>
#include <scsi.h>
#include <serial.h>
#include <stdio_dev.h>
#include <timer.h>
#include <trace.h>
#include <watchdog.h>
#include <xen.h>
#include <asm/sections.h>
#include <dm/root.h>
#include <dm/ofnode.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <wdt.h>
#include <asm-generic/gpio.h>
#include <relocate.h>

DECLARE_GLOBAL_DATA_PTR;

ulong monitor_flash_len;

__weak int board_flash_wp_on(void)
{
	/*
	 * Most flashes can't be detected when write protection is enabled,
	 * so provide a way to let U-Boot gracefully ignore write protected
	 * devices.
	 */
	return 0;
}

__weak int cpu_secondary_init_r(void)
{
	return 0;
}

static int initr_trace(void)
{
#ifdef CONFIG_TRACE
	trace_init(gd->trace_buff, CONFIG_TRACE_BUFFER_SIZE);
#endif

	return 0;
}

static int initr_reloc(void)
{
	/* tell others: relocation done */
	gd->flags |= GD_FLG_RELOC | GD_FLG_FULL_MALLOC_INIT;

	return 0;
}

#if defined(CONFIG_ARM) || defined(CONFIG_RISCV)
/*
 * Some of these functions are needed purely because the functions they
 * call return void. If we change them to return 0, these stubs can go away.
 */
static int initr_caches(void)
{
	/* Enable caches */
	enable_caches();
	return 0;
}
#endif

__weak int fixup_cpu(void)
{
	return 0;
}

static int initr_reloc_global_data(void)
{
#ifdef __ARM__
	monitor_flash_len = _end - __image_copy_start;
#elif defined(CONFIG_RISCV)
	monitor_flash_len = (ulong)_end - (ulong)_start;
#elif !defined(CONFIG_SANDBOX) && !defined(CONFIG_NIOS2)
	monitor_flash_len = (ulong)__init_end - gd->relocaddr;
#endif
#if defined(CONFIG_MPC85xx) || defined(CONFIG_MPC86xx)
	/*
	 * The gd->cpu pointer is set to an address in flash before relocation.
	 * We need to update it to point to the same CPU entry in RAM.
	 * TODO: why not just add gd->reloc_ofs?
	 */
	gd->arch.cpu += gd->relocaddr - CONFIG_SYS_MONITOR_BASE;

	/*
	 * If we didn't know the cpu mask & # cores, we can save them of
	 * now rather than 'computing' them constantly
	 */
	fixup_cpu();
#endif
#ifdef CONFIG_ENV_RELOC_GD_ENV_ADDR
	/*
	 * Relocate the early env_addr pointer unless we know it is not inside
	 * the binary. Some systems need this and for the rest, it doesn't hurt.
	 */
	gd->env_addr += gd->reloc_off;
#endif

	/*
	 * For CONFIG_OF_EMBED case the FDT is embedded into ELF, available by
	 * __dtb_dt_begin. After U-Boot ELF self-relocation to RAM top address
	 * it is worth to update fdt_blob in global_data
	 */
	if (IS_ENABLED(CONFIG_OF_EMBED))
		fdtdec_setup_embed();

#ifdef CONFIG_EFI_LOADER
	/*
	 * On the ARM architecture gd is mapped to a fixed register (r9 or x18).
	 * As this register may be overwritten by an EFI payload we save it here
	 * and restore it on every callback entered.
	 */
	efi_save_gd();

	if (!(gd->flags & GD_FLG_SKIP_RELOC))
		efi_runtime_relocate(gd->relocaddr, NULL);

#endif
	/*
	 * We are done with all relocations change the permissions of the binary
	 * NOTE: __start_rodata etc are defined in arm64 linker scripts and
	 * sections.h. If you want to add support for your platform you need to
	 * add the symbols on your linker script, otherwise they will point to
	 * random addresses.
	 *
	 */
	if (IS_ENABLED(CONFIG_MMU_PGPROT)) {
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_rodata),
				 (size_t)(uintptr_t)(__end_rodata - __start_rodata),
				 MMU_ATTR_RO);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_data),
				 (size_t)(uintptr_t)(__end_data - __start_data),
				 MMU_ATTR_RW);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__text_start),
				 (size_t)(uintptr_t)(__text_end - __text_start),
				 MMU_ATTR_RX);
	}

	return 0;
}

__weak int arch_initr_trap(void)
{
	return 0;
}

#if defined(CONFIG_SYS_INIT_RAM_LOCK) && defined(CONFIG_E500)
static int initr_unlock_ram_in_cache(void)
{
	unlock_ram_in_cache();	/* it's time to unlock D-cache in e500 */
	return 0;
}
#endif

static int initr_barrier(void)
{
#ifdef CONFIG_PPC
	/* TODO: Can we not use dmb() macros for this? */
	asm("sync ; isync");
#endif
	return 0;
}

static int initr_malloc(void)
{
	ulong start;

	WATCHDOG_RESET();
	qcom_ramoops_early_log("INIT: initr_malloc start\n");
#if CONFIG_IS_ENABLED(SYS_MALLOC_F)
	debug("Pre-reloc malloc() used %#x bytes (%d KB)\n", gd->malloc_ptr,
	      gd->malloc_ptr / 1024);
#endif
	/* The malloc area is immediately below the monitor copy in DRAM */
	/*
	 * This value MUST match the value of gd->start_addr_sp in board_f.c:
	 * reserve_noncached().
	 */
	start = gd->relocaddr - TOTAL_MALLOC_LEN;
	gd_set_malloc_start(start);
	WATCHDOG_RESET();
	mem_malloc_init(start, TOTAL_MALLOC_LEN);
	WATCHDOG_RESET();
	qcom_ramoops_early_log("INIT: initr_malloc done\n");
	return 0;
}

static int initr_of_live(void)
{
	if (CONFIG_IS_ENABLED(OF_LIVE)) {
		int ret;

		bootstage_start(BOOTSTAGE_ID_ACCUM_OF_LIVE, "of_live");
		ret = of_live_build(gd->fdt_blob,
				    (struct device_node **)gd_of_root_ptr());
		bootstage_accum(BOOTSTAGE_ID_ACCUM_OF_LIVE);
		if (ret)
			return ret;
	}

	return 0;
}

#ifdef CONFIG_DM
static bool s_enable_4s_test = false;

static int initr_dm(void)
{
	extern void qcom_ramoops_early_log(const char *msg);

	qcom_ramoops_early_log("[DM 0] oftree_reset\n");
	oftree_reset();

	qcom_ramoops_early_log("[TEST v40] Enabling per-function 4s delay test from initr_dm onwards!\n");
	s_enable_4s_test = true;
	return 0;
}
#endif

static int initr_dm_devices(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_TIMER_EARLY)) {
		ret = dm_timer_init();
		if (ret)
			return ret;
	}

	if (IS_ENABLED(CONFIG_MULTIPLEXER)) {
		/*
		 * Initialize the multiplexer controls to their default state.
		 * This must be done early as other drivers may unknowingly
		 * rely on it.
		 */
		ret = dm_mux_init();
		if (ret)
			return ret;
	}

	return 0;
}

static int initr_bootstage(void)
{
	bootstage_mark_name(BOOTSTAGE_ID_START_UBOOT_R, "board_init_r");

	return 0;
}

__weak int power_init_board(void)
{
	return 0;
}

static int initr_announce(void)
{
	debug("Now running in RAM - U-Boot at: %08lx\n", gd->relocaddr);
	return 0;
}

static int __maybe_unused initr_binman(void)
{
	int ret;

	ret = binman_init();
	if (ret)
		printf("binman_init failed:%d\n", ret);

	return ret;
}

#if defined(CONFIG_MTD_NOR_FLASH)
__weak int is_flash_available(void)
{
	return 1;
}

static int initr_flash(void)
{
	ulong flash_size = 0;
	struct bd_info *bd = gd->bd;

	if (!is_flash_available())
		return 0;

	puts("Flash: ");

	if (board_flash_wp_on())
		printf("Uninitialized - Write Protect On\n");
	else
		flash_size = flash_init();

	print_size(flash_size, "");
#ifdef CONFIG_SYS_FLASH_CHECKSUM
	/*
	 * Compute and print flash CRC if flashchecksum is set to 'y'
	 *
	 * NOTE: Maybe we should add some schedule()? XXX
	 */
	if (env_get_yesno("flashchecksum") == 1) {
		const uchar *flash_base = (const uchar *)CFG_SYS_FLASH_BASE;

		printf("  CRC: %08X", crc32(0,
					    flash_base,
					    flash_size));
	}
#endif /* CONFIG_SYS_FLASH_CHECKSUM */
	putc('\n');

	/* update start of FLASH memory    */
#ifdef CFG_SYS_FLASH_BASE
	bd->bi_flashstart = CFG_SYS_FLASH_BASE;
#endif
	/* size of FLASH memory (final value) */
	bd->bi_flashsize = flash_size;

#if defined(CONFIG_SYS_UPDATE_FLASH_SIZE)
	/* Make a update of the Memctrl. */
	update_flash_size(flash_size);
#endif

#if defined(CONFIG_OXC) || defined(CONFIG_RMU)
	/* flash mapped at end of memory map */
	bd->bi_flashoffset = CONFIG_TEXT_BASE + flash_size;
#elif CONFIG_SYS_MONITOR_BASE == CFG_SYS_FLASH_BASE
	bd->bi_flashoffset = monitor_flash_len;	/* reserved area for monitor */
#endif
	return 0;
}
#endif

#ifdef CONFIG_CMD_NAND
/* go init the NAND */
static int initr_nand(void)
{
	puts("NAND:  ");
	nand_init();
	printf("%lu MiB\n", nand_size() / 1024);
	return 0;
}
#endif

#if defined(CONFIG_CMD_ONENAND)
/* go init the NAND */
static int initr_onenand(void)
{
	puts("NAND:  ");
	onenand_init();
	return 0;
}
#endif

#ifdef CONFIG_MMC
static int initr_mmc(void)
{
	puts("MMC:   ");
	mmc_initialize(gd->bd);
	return 0;
}
#endif

#ifdef CONFIG_PVBLOCK
static int initr_pvblock(void)
{
	puts("PVBLOCK: ");
	pvblock_init();
	return 0;
}
#endif

/*
 * Tell if it's OK to load the environment early in boot.
 *
 * If CONFIG_OF_CONTROL is defined, we'll check with the FDT to see
 * if this is OK (defaulting to saying it's OK).
 *
 * NOTE: Loading the environment early can be a bad idea if security is
 *       important, since no verification is done on the environment.
 *
 * Return: 0 if environment should not be loaded, !=0 if it is ok to load
 */
static int should_load_env(void)
{
	if (IS_ENABLED(CONFIG_OF_CONTROL))
		return ofnode_conf_read_int("load-environment", 1);

	return 1;
}

static int initr_env(void)
{
	/* initialize environment */
	if (should_load_env())
		env_relocate();
	else
		env_set_default(NULL, 0);

	env_import_fdt();

	if (IS_ENABLED(CONFIG_OF_CONTROL))
		env_set_hex("fdtcontroladdr",
			    (unsigned long)map_to_sysmem(gd->fdt_blob));

	#if (IS_ENABLED(CONFIG_SAVE_PREV_BL_INITRAMFS_START_ADDR) || \
						IS_ENABLED(CONFIG_SAVE_PREV_BL_FDT_ADDR))
		save_prev_bl_data();
	#endif

	/* Initialize from environment */
	image_load_addr = env_get_ulong("loadaddr", 16, image_load_addr);

	return 0;
}

#ifdef CONFIG_SYS_MALLOC_BOOTPARAMS
static int initr_malloc_bootparams(void)
{
	gd->bd->bi_boot_params = (ulong)malloc(CONFIG_SYS_BOOTPARAMS_LEN);
	if (!gd->bd->bi_boot_params) {
		puts("WARNING: Cannot allocate space for boot parameters\n");
		return -ENOMEM;
	}
	return 0;
}
#endif

static int initr_boot_led_blink(void)
{
	led_boot_blink();

	return 0;
}

static int initr_boot_led_on(void)
{
	led_boot_on();

	return 0;
}

#if CONFIG_IS_ENABLED(NET)
static int initr_net(void)
{
	puts("Net:   ");
	eth_initialize();
#if defined(CONFIG_RESET_PHY_R)
	debug("Reset Ethernet PHY\n");
	reset_phy();
#endif
	return 0;
}
#endif

#ifdef CONFIG_POST
static int initr_post(void)
{
	post_run(NULL, POST_RAM | post_bootmode_get(0));
	return 0;
}
#endif

#if defined(CFG_PRAM)
/*
 * Export available size of memory for Linux, taking into account the
 * protected RAM at top of memory
 */
int initr_mem(void)
{
	ulong pram = 0;
	char memsz[32];

	pram = env_get_ulong("pram", 10, CFG_PRAM);
	sprintf(memsz, "%ldk", (long int)((gd->ram_size / 1024) - pram));
	env_set("mem", memsz);

	return 0;
}
#endif

static int initr_lmb(void)
{
	qcom_ramoops_early_log("LMB: enter initr_lmb\n");
	if (CONFIG_IS_ENABLED(LMB)) {
		int ret = lmb_init();
		qcom_ramoops_early_log("LMB: lmb_init returned\n");
		return ret;
	} else {
		return 0;
	}
}

static int dm_announce(void)
{
	int device_count;
	int uclass_count;

	qcom_ramoops_early_log("[DM-ANN 0] Inside dm_announce\n");
	if (IS_ENABLED(CONFIG_DM)) {
		qcom_ramoops_early_log("[DM-ANN 1] Calling dm_get_stats\n");
		dm_get_stats(&device_count, &uclass_count);
		char buf[128];
		snprintf(buf, sizeof(buf), "[DM-ANN 2] dm_get_stats: %d devices, %d uclasses\n",
			 device_count, uclass_count);
		qcom_ramoops_early_log(buf);

		printf("Core:  %d devices, %d uclasses", device_count,
		       uclass_count);
		if (CONFIG_IS_ENABLED(OF_REAL))
			printf(", devicetree: %s", fdtdec_get_srcname());
		if (CONFIG_IS_ENABLED(UPL))
			printf(", universal payload active");
		printf("\n");
		if (IS_ENABLED(CONFIG_OF_HAS_PRIOR_STAGE) &&
		    (gd->fdt_src == FDTSRC_SEPARATE ||
		     gd->fdt_src == FDTSRC_EMBED)) {
			printf("Warning: Unexpected devicetree source (not from a prior stage)");
			printf("Warning: U-Boot may not function properly\n");
		}
	}
	qcom_ramoops_early_log("[DM-ANN 3] dm_announce completed!\n");

	return 0;
}

static int run_main_loop(void)
{
	int ret;
	extern void qcom_ramoops_early_log(const char *msg);
	extern void early_delay_s(uint32_t sec);

	qcom_ramoops_early_log("[4S-TEST] Entering run_main_loop, starting 4s delay...\n");
	for (int _t = 1; _t <= 4; _t++) {
		early_delay_s(1);
		char _dbuf[64];
		snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] run_main_loop tick %d/4s OK\n", _t);
		qcom_ramoops_early_log(_dbuf);
	}
	qcom_ramoops_early_log("[4S-TEST] run_main_loop 4s delay done!\n");

#ifdef CONFIG_SANDBOX
	sandbox_main_loop_init();
#endif

	ret = event_notify_null(EVT_MAIN_LOOP);
	if (ret)
		return ret;

	/* main_loop() can return to retry autoboot, if so just run it again */
	for (;;)
		main_loop();
	return 0;
}

#undef INITCALL
#define INITCALL(_call) \
	do { \
		extern void qcom_ramoops_early_log(const char *msg); \
		extern void early_delay_s(uint32_t sec); \
		char _dbuf[128]; \
		qcom_ramoops_early_log("INIT: " #_call "\n"); \
		if (_call()) { \
			qcom_ramoops_early_log("FAIL: " #_call "\n"); \
			hang(); \
		} \
		if (s_enable_4s_test) { \
			snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] %s OK, starting 4s delay...\n", #_call); \
			qcom_ramoops_early_log(_dbuf); \
			for (int _t = 1; _t <= 4; _t++) { \
				early_delay_s(1); \
				snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] %s tick %d/4s OK\n", #_call, _t); \
				qcom_ramoops_early_log(_dbuf); \
			} \
			snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] %s 4s delay done!\n", #_call); \
			qcom_ramoops_early_log(_dbuf); \
		} \
	} while (0)

#undef INITCALL_EVT
#define INITCALL_EVT(_evt) \
	do { \
		extern void qcom_ramoops_early_log(const char *msg); \
		extern void early_delay_s(uint32_t sec); \
		char _dbuf[128]; \
		snprintf(_dbuf, sizeof(_dbuf), "INIT_EVT: %s\n", event_type_name(_evt)); \
		qcom_ramoops_early_log(_dbuf); \
		if (event_notify_null(_evt)) { \
			printf("%s(): event %d/%s failed\n", __func__, _evt, \
			       event_type_name(_evt)) ; \
			hang(); \
		} \
		if (s_enable_4s_test) { \
			snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] EVT %s OK, starting 4s delay...\n", event_type_name(_evt)); \
			qcom_ramoops_early_log(_dbuf); \
			for (int _t = 1; _t <= 4; _t++) { \
				early_delay_s(1); \
				snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] EVT %s tick %d/4s OK\n", event_type_name(_evt), _t); \
				qcom_ramoops_early_log(_dbuf); \
			} \
			snprintf(_dbuf, sizeof(_dbuf), "[4S-TEST] EVT %s 4s delay done!\n", event_type_name(_evt)); \
			qcom_ramoops_early_log(_dbuf); \
		} \
	} while (0)

static void initcall_run_r(void)
{
	/*
	 * Please do not add logic to this function (variables, if (), etc.).
	 * For simplicity it should remain an ordered list of function calls.
	 */
	INITCALL(initr_trace);
	INITCALL(initr_reloc);
	INITCALL(event_init);
	/* TODO: could x86/PPC have this also perhaps? */
#if CONFIG_IS_ENABLED(ARM) || CONFIG_IS_ENABLED(RISCV)
	INITCALL(initr_caches);
	/* Note: For Freescale LS2 SoCs, new MMU table is created in DDR.
	 *	 A temporary mapping of IFC high region is since removed,
	 *	 so environmental variables in NOR flash is not available
	 *	 until board_init() is called below to remap IFC to high
	 *	 region.
	 */
#endif
	INITCALL(initr_reloc_global_data);
#if CONFIG_IS_ENABLED(SYS_INIT_RAM_LOCK) && CONFIG_IS_ENABLED(E500)
	INITCALL(initr_unlock_ram_in_cache);
#endif
	INITCALL(initr_barrier);
	INITCALL(initr_malloc);
	INITCALL(log_init);
	INITCALL(initr_bootstage); /* Needs malloc() but has its own timer */
#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	INITCALL(console_record_init);
#endif
#if CONFIG_IS_ENABLED(SYS_HAS_NONCACHED_MEMORY)
	INITCALL(noncached_init);
#endif
	INITCALL(initr_of_live);
#if CONFIG_IS_ENABLED(DM)
	INITCALL(initr_dm);
#endif
#if CONFIG_IS_ENABLED(ADDR_MAP)
	INITCALL(init_addr_map);
#endif
#if CONFIG_IS_ENABLED(BOARD_INIT)
	INITCALL(board_init);	/* Setup chipselects */
	qcom_ramoops_early_log("[MAJOR 2] board_init done\n");
	qcom_ramoops_early_log("[CKPT 4] board_init OK\n");
#endif
	/*
	 * TODO: printing of the clock inforamtion of the board is now
	 * implemented as part of bdinfo command. Currently only support for
	 * davinci SOC's is added. Remove this check once all the board
	 * implement this.
	 */
#if CONFIG_IS_ENABLED(CLOCKS)
	INITCALL(set_cpu_clk_info);
#endif
	INITCALL(initr_lmb);
	qcom_ramoops_early_log("[R 2] initr_lmb done\n");
#if CONFIG_IS_ENABLED(EFI_LOADER)
	INITCALL(efi_memory_init);
#endif
#if CONFIG_IS_ENABLED(BINMAN_FDT)
	INITCALL(initr_binman);
#endif
#if CONFIG_IS_ENABLED(FSP_VERSION2)
	INITCALL(arch_fsp_init_r);
#endif
	INITCALL(initr_dm_devices);
	qcom_ramoops_early_log("[SUB 5] initr_dm_devices done\n");
	qcom_ramoops_early_log("[CKPT 5] initr_dm_devices OK\n");
	INITCALL(stdio_init_tables);
	qcom_ramoops_early_log("[R 4] stdio_init_tables done\n");
	INITCALL(serial_initialize);
	qcom_ramoops_early_log("[SUB 6] serial_initialize done\n");
	qcom_ramoops_early_log("[CKPT 6] serial_initialize OK\n");
	INITCALL(initr_announce);
	INITCALL(dm_announce);
	qcom_ramoops_early_log("[CKPT 7] dm_announce OK\n");
#if CONFIG_IS_ENABLED(WDT)
	INITCALL(initr_watchdog);
#endif
	WATCHDOG_RESET();
	INITCALL(arch_initr_trap);
#if CONFIG_IS_ENABLED(BOARD_EARLY_INIT_R)
	INITCALL(board_early_init_r);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(POST)
	INITCALL(post_output_backlog);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(PCI_INIT_R) && CONFIG_IS_ENABLED(SYS_EARLY_PCI_INIT)
	/*
	 * Do early PCI configuration _before_ the flash gets initialised,
	 * because PCU resources are crucial for flash access on some boards.
	 */
	INITCALL(pci_init);
#endif
#if CONFIG_IS_ENABLED(ARCH_EARLY_INIT_R)
	INITCALL(arch_early_init_r);
#endif
	INITCALL(power_init_board);
#if CONFIG_IS_ENABLED(MTD_NOR_FLASH)
	INITCALL(initr_flash);
#endif
	WATCHDOG_RESET();
#if IS_ENABLED(CONFIG_PPC) || CONFIG_IS_ENABLED(M68K) || CONFIG_IS_ENABLED(X86)
	/* initialize higher level parts of CPU like time base and timers */
	INITCALL(cpu_init_r);
#endif
#if CONFIG_IS_ENABLED(EFI_LOADER)
	INITCALL(efi_init_early);
#endif
#if CONFIG_IS_ENABLED(CMD_NAND)
	INITCALL(initr_nand);
#endif
#if CONFIG_IS_ENABLED(CMD_ONENAND)
	INITCALL(initr_onenand);
#endif
#if CONFIG_IS_ENABLED(MMC)
	INITCALL(initr_mmc);
#endif
#if CONFIG_IS_ENABLED(XEN)
	INITCALL(xen_init);
#endif
#if CONFIG_IS_ENABLED(PVBLOCK)
	INITCALL(initr_pvblock);
#endif
	INITCALL(initr_env);
	qcom_ramoops_early_log("[SUB 7] initr_env done\n");
	qcom_ramoops_early_log("[CKPT 7] initr_env OK\n");
#if CONFIG_IS_ENABLED(SYS_MALLOC_BOOTPARAMS)
	INITCALL(initr_malloc_bootparams);
#endif
	WATCHDOG_RESET();
	INITCALL(cpu_secondary_init_r);
#if CONFIG_IS_ENABLED(ID_EEPROM)
	INITCALL(mac_read_from_eeprom);
#endif
	INITCALL_EVT(EVT_SETTINGS_R);
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(PCI_INIT_R) && !CONFIG_IS_ENABLED(SYS_EARLY_PCI_INIT)
	/*
	 * Do pci configuration
	 */
	INITCALL(pci_init);
#endif
	INITCALL(stdio_add_devices);
	qcom_ramoops_early_log("[MAJOR 3] stdio_add_devices done (video probed)\n");
	qcom_ramoops_early_log("[CKPT 8] stdio_add_devices OK\n");
	INITCALL(jumptable_init);
#if CONFIG_IS_ENABLED(API)
	INITCALL(api_init);
#endif
	INITCALL(console_init_r);	/* fully init console as a device */
	qcom_ramoops_early_log("[MAJOR 4] console_init_r done (VIDCONSOLE ACTIVE)\n");
	qcom_ramoops_early_log("[CKPT 9] console_init_r OK\n");
#if CONFIG_IS_ENABLED(DISPLAY_BOARDINFO_LATE)
	INITCALL(console_announce_r);
	INITCALL(show_board_info);
#endif
	/* miscellaneous arch-dependent init */
#if CONFIG_IS_ENABLED(ARCH_MISC_INIT)
	INITCALL(arch_misc_init);
#endif
	/* miscellaneous platform-dependent init */
#if CONFIG_IS_ENABLED(MISC_INIT_R)
	INITCALL(misc_init_r);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(CMD_KGDB)
	INITCALL(kgdb_init);
#endif
	INITCALL(interrupt_init);
#if defined(CONFIG_MICROBLAZE) || defined(CONFIG_M68K)
	INITCALL(timer_init);		/* initialize timer */
#endif
	INITCALL(initr_boot_led_blink);
	/* PPC has a udelay(20) here dating from 2002. Why? */
#if CONFIG_IS_ENABLED(BOARD_LATE_INIT)
	INITCALL(board_late_init);
	qcom_ramoops_early_log("[SUB 8] board_late_init done\n");
	qcom_ramoops_early_log("[CKPT 10] board_late_init OK\n");
#endif
	qcom_ramoops_early_log("[SUB 8.1] After board_late_init\n");
#if CONFIG_IS_ENABLED(PCI_ENDPOINT)
	INITCALL(pci_ep_init);
#endif
#if CONFIG_IS_ENABLED(NET)
	WATCHDOG_RESET();
	INITCALL(initr_net);
#endif
#if CONFIG_IS_ENABLED(POST)
	INITCALL(initr_post);
#endif
	WATCHDOG_RESET();
	qcom_ramoops_early_log("[SUB 8.2] Calling EVT_LAST_STAGE_INIT\n");
	INITCALL_EVT(EVT_LAST_STAGE_INIT);
	qcom_ramoops_early_log("[SUB 8.3] After EVT_LAST_STAGE_INIT\n");
#if defined(CFG_PRAM)
	INITCALL(initr_mem);
#endif
	INITCALL(initr_boot_led_on);
	qcom_ramoops_early_log("[R 10] Entering run_main_loop (SHELL REACHED!)\n");
	INITCALL(run_main_loop);
}

void board_init_r(gd_t *new_gd, ulong dest_addr)
{
	/*
	 * The pre-relocation drivers may be using memory that has now gone
	 * away. Mark serial as unavailable - this will fall back to the debug
	 * UART if available.
	 *
	 * Do the same with log drivers since the memory may not be available.
	 */
	gd->flags &= ~(GD_FLG_SERIAL_READY | GD_FLG_LOG_READY);
	gd->cur_serial_dev = NULL;

	/*
	 * Set up the new global data pointer. So far only x86 does this
	 * here.
	 * TODO(sjg@chromium.org): Consider doing this for all archs, or
	 * dropping the new_gd parameter.
	 */
	if (CONFIG_IS_ENABLED(X86_64) && !IS_ENABLED(CONFIG_EFI_APP))
		arch_setup_gd(new_gd);

#if defined(CONFIG_RISCV)
	set_gd(new_gd);
#elif !defined(CONFIG_X86) && !defined(CONFIG_ARM) && !defined(CONFIG_ARM64)
	gd = new_gd;
#endif
	gd->flags &= ~GD_FLG_LOG_READY;

	initcall_run_r();

	/* NOTREACHED - run_main_loop() does not return */
	hang();
}
