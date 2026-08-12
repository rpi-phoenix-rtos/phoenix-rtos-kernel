/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Hardware Abstraction Layer (AArch64)
 *
 * Copyright 2014, 2018, 2024 Phoenix Systems
 * Author: Pawel Pisarczyk, Jacek Maksymowicz
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "hal/console.h"
#include "dtb.h"

#include "syspage.h"
#include "halsyspage.h"
#include "arch/pmap.h"

#include "lib/assert.h"

#include <board_config.h>

static struct {
	int started;
} hal_common;

/* parasoft-begin-suppress MISRAC2012-RULE_8_4 "Definition in assembly" */
syspage_t *hal_syspage;
u64 relOffs;
u32 schedulerLocked;
/* parasoft-end-suppress MISRAC2012-RULE_8_4 */


void *hal_syspageRelocate(void *data)
{
	return ((u8 *)data + relOffs);
}


ptr_t hal_syspageAddr(void)
{
	return (ptr_t)hal_syspage;
}


int hal_started(void)
{
	return hal_common.started;
}


void _hal_start(void)
{
	hal_common.started = 1;
}


void hal_lockScheduler(void)
{
#if NUM_CPUS != 1
	/* clang-format off */
	__asm__ volatile (
		"mov w1, #1\n"
		"b 2f\n"
	"1:\n"
		"wfe\n"
	"2:\n"
		"ldaxr w2, [%0]\n"
		"cbnz w2, 1b\n"
		"stxr w2, w1, [%0]\n"
		"cbnz w2, 2b\n"
	:
	: "r" (&schedulerLocked)
	: "x1", "x2", "memory");
	/* clang-format on */
#else
	/* Not necessary on single-core systems */
	(void)schedulerLocked;
	return;
#endif
}


void _hal_init_c(void) __attribute__((section(".init"), noinline));


void _hal_init_c(void)
{
	const syspage_prog_t *dtb;
	addr_t dtbStart;
	addr_t dtbEnd;

	/* dtbEnd is read from the real DTB size (firmwareDtbSize or dtb->end).
	 * An earlier Pi 4 cache-coherency workaround faked it as
	 * dtbStart + 0x10000, which could silently truncate parsing of a DTB
	 * larger than 64 KiB; that workaround was removed 2026-05-17 once the
	 * armstub cache fix landed. See TD-04. */

	hal_common.started = 0;
	schedulerLocked = 0;
	_hal_spinlockInit();

	if ((hal_syspage->hs.firmwareDtb != 0u) && (hal_syspage->hs.firmwareDtbSize != 0u)) {
		dtbStart = hal_syspage->hs.firmwareDtb;
		dtbEnd = dtbStart + hal_syspage->hs.firmwareDtbSize;
	}
	else {
		dtb = syspage_progNameResolve("system.dtb");
		if (dtb == NULL) {
#ifdef NDEBUG
			hal_cpuReboot();
#else
			for (;;) {
				hal_cpuHalt();
			}
#endif
		}

		dtbStart = dtb->start;
		dtbEnd = dtb->end;
	}

	_pmap_preinit(dtbStart, dtbEnd);
	_hal_platformInit();
	_hal_consoleInit();
	_hal_exceptionsInit();
	_hal_interruptsInit();
	_hal_cpuInit();
	_hal_timerInit(SYSTICK_INTERVAL);

	return;
}
