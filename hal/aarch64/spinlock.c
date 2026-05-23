/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Spinlock
 *
 * Copyright 2024 Phoenix Systems
 * Author: Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/spinlock.h"
#include "hal/cpu.h"
#include "hal/hal.h"
#include "hal/list.h"

static struct {
	spinlock_t spinlock;
	spinlock_t *first;
} spinlock_common;


void hal_spinlockSet(spinlock_t *spinlock, spinlock_ctx_t *sc)
{
#if NUM_CPUS == 1
	(void)spinlock;
	__asm__ volatile(
		"mrs x2, daif\n"
		"msr daifSet, #3\n"
		"str w2, [%0]\n"
		:
		: "r"(sc)
		: "x2", "memory");
	return;
#else
	/* SMP-D-6 (2026-05-23): the prior `hal_started()` gate that
	 * fell back to a fake (daif-only) spinlock during early boot
	 * was unsafe in SMP mode — once secondaries are released and
	 * start taking timer IRQs (via the `daifClr #7` at the bottom
	 * of `_other_core_virtual`), both cpu0 and the secondaries
	 * enter `interrupts_dispatch` and race on
	 * `interrupts_common.counters[]` / `handlers[]` with no real
	 * mutual exclusion. Always use the ldaxr/stxr real-lock path
	 * regardless of hal_started — the lock itself is initialised
	 * by `_hal_spinlockCreate` which `_hal_init_c` runs before
	 * any secondary is released, so the lock byte is always 1
	 * (unlocked) by the time anyone tries to grab it. */
	/* clang-format off */
	__asm__ volatile (
		"mrs x2, daif\n"
		"msr daifSet, #3\n"
		"str w2, [%0]\n"
		"b 2f\n"
	"1:\n"
		"wfe\n"
	"2:\n"
		"ldaxrb w2, [%1]\n"
		"cbz w2, 1b\n"
		"stxrb w2, wzr, [%1]\n"
		"cbnz w2, 2b\n"
	:
	: "r" (sc), "r" (&spinlock->lock)
	: "x2", "memory");
	/* clang-format on */
#endif
}


void hal_spinlockClear(spinlock_t *spinlock, spinlock_ctx_t *sc)
{
#if NUM_CPUS == 1
	(void)spinlock;
	__asm__ volatile(
		"ldr w2, [%0]\n"
		"msr daif, x2\n"
		:
		: "r"(sc)
		: "x2", "memory");
	return;
#else
	/* SMP-D-6: see hal_spinlockSet — same `hal_started()` gate
	 * removal applies here. Always release the real lock with
	 * stlrb so the global monitor wakes other CPUs in wfe. */
	/* clang-format off */
	__asm__ volatile (
		"mov w2, #1\n"
		"stlrb w2, [%0]\n" /* Global monitor clear generates an event, SEV not necessary */
		"ldr w2, [%1]\n"
		"msr daif, x2\n"
	:
	: "r" (&spinlock->lock), "r" (sc)
	: "x2", "memory");
	/* clang-format on */
#endif
}


static void _hal_spinlockCreate(spinlock_t *spinlock, const char *name)
{
	spinlock->lock = 1;
	spinlock->name = name;
	HAL_LIST_ADD(&spinlock_common.first, spinlock);
}


void hal_spinlockCreate(spinlock_t *spinlock, const char *name)
{
	spinlock_ctx_t sc;

	if (hal_started() == 0) {
		/* Early boot is single-core with interrupts masked. Avoid taking the
		 * global spinlock registry lock before the AArch64 cache/exclusive
		 * monitor setup is fully proven on real Pi 4 hardware. */
		_hal_spinlockCreate(spinlock, name);
		return;
	}

	hal_spinlockSet(&spinlock_common.spinlock, &sc);
	_hal_spinlockCreate(spinlock, name);
	hal_spinlockClear(&spinlock_common.spinlock, &sc);
}


void hal_spinlockDestroy(spinlock_t *spinlock)
{
	spinlock_ctx_t sc;

	hal_spinlockSet(&spinlock_common.spinlock, &sc);

	HAL_LIST_REMOVE(&spinlock_common.first, spinlock);

	hal_spinlockClear(&spinlock_common.spinlock, &sc);
}


__attribute__((section(".init"))) void _hal_spinlockInit(void)
{
	spinlock_common.first = NULL;
	_hal_spinlockCreate(&spinlock_common.spinlock, "spinlock_common.spinlock");
}
