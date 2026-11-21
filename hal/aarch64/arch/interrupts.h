/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Exception and interrupt handling
 *
 * Copyright 2016, 2018, 2020, 2024 Phoenix Systems
 * Author: Pawel Pisarczyk, Jacek Maksymowicz
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_HAL_AARCH64_INTERRUPTS_H_
#define _PH_HAL_AARCH64_INTERRUPTS_H_

#include "cpu.h"

typedef struct _intr_handler_t {
	struct _intr_handler_t *next;
	struct _intr_handler_t *prev;
	unsigned int n;
	intrFn_t f;
	void *data;
} intr_handler_t;


enum { TIMER_WAKEUP_IRQ = 1U };

/* SGI that makes the receiving CPU run the scheduler: wakes an idle secondary
 * for a thread that just became ready (KERNEL_TICKLESS_IDLE, proc/threads.c).
 * A macro rather than an enum entry so that code can test it with #ifdef. */
#define RESCHED_IRQ 2U


#endif
