/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _U_BOOT_SCHEDULE_H
#define _U_BOOT_SCHEDULE_H

#include <uthread.h>

#if CONFIG_IS_ENABLED(CYCLIC)
/**
 * schedule() - Schedule all potentially waiting tasks
 *
 * Run all pending tasks registered via the cyclic framework, and
 * potentially perform other actions that need to be done
 * periodically.
 */
void schedule(void);

#else

static inline void schedule(void)
{
	*(volatile uint32_t *)0x0b017010ULL = 0x7FFFFFFF;
	*(volatile uint32_t *)0x0b017014ULL = 0x7FFFFFFF;
	*(volatile uint32_t *)0x0b017004ULL = 1;
	*(volatile uint32_t *)0x0b017008ULL = 1;
	asm volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)0x0b017004ULL = 0;
	*(volatile uint32_t *)0x0b017008ULL = 0;
	asm volatile("dsb sy" ::: "memory");
	uthread_schedule();
}

#endif

#endif
