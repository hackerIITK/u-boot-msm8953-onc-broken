/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 */

#ifndef __INITCALL_H
#define __INITCALL_H

#include <asm/types.h>
#include <event.h>
#include <hang.h>

_Static_assert(EVT_COUNT < 256, "Can only support 256 event types with 8 bits");

#define INITCALL(_call) \
	do { \
		extern void qcom_ramoops_early_log(const char *msg); \
		qcom_ramoops_early_log("INIT: " #_call "\n"); \
		if (_call()) { \
			qcom_ramoops_early_log("FAIL: " #_call "\n"); \
			hang(); \
		} \
	} while (0)

#define INITCALL_EVT(_evt) \
	do { \
		if (event_notify_null(_evt)) { \
			printf("%s(): event %d/%s failed\n", __func__, _evt, \
			       event_type_name(_evt)) ; \
			hang(); \
		} \
	} while (0)

#if defined(CONFIG_WATCHDOG) || defined(CONFIG_HW_WATCHDOG)
#define WATCHDOG_INIT() INITCALL(init_func_watchdog_init)
#define WATCHDOG_RESET() INITCALL(init_func_watchdog_reset)
#else
#define WATCHDOG_INIT()
#define WATCHDOG_RESET()
#endif

#endif
