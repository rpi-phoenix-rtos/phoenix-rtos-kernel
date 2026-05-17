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
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/hal.h"
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

	/* TD-04-hack-2 + TD-04-hack-3 cleaned up 2026-05-17.
	 *
	 * Pre-cleanup state: this function had ~17 inline marker stores
	 * (H, 4, 5, 6, F/S, r/D, s, E, 7, 8, 9, a, b, c, d, e) to a
	 * TTBR1-mapped early-UART pointer at 0xffffffffffe00000, plus a
	 * faked `dtbEnd = dtbStart + 0x10000` because reading `dtb->end`
	 * hung the kernel on real Pi 4 silicon in the cache-off era.
	 *
	 * With the 2026-05-17 armstub fix (CPUACTLR_EL1[46] erratum 1319367
	 * + L2CTLR_EL1 BCM2711 timing) the underlying cache-coherency
	 * defect is gone; the Heisenbug-shaped hangs that originally
	 * justified the markers and the fake `dtbEnd` no longer reproduce.
	 *
	 * Markers stripped to reduce UART chatter (helps boot speed —
	 * every byte to UART also gets mirrored to HDMI by pl011-tty's
	 * fbcon path; fewer bytes = faster). Real `dtb->end` read
	 * restored — correctness improvement; the prior 64 KiB cap could
	 * silently truncate parsing for any DTB > 64 KiB (Pi 4's is
	 * currently 56 KiB so the hack was within bounds, but a future
	 * Pi variant or kernel cmdline addition could push it over). */

	hal_common.started = 0;
	schedulerLocked = 0;
	_hal_spinlockInit();

	if ((hal_syspage->hs.firmwareDtb != 0u) && (hal_syspage->hs.firmwareDtbSize != 0u)) {
		dtbStart = hal_syspage->hs.firmwareDtb;
		dtbEnd = dtbStart + hal_syspage->hs.firmwareDtbSize;
		hal_consolePrint(ATTR_USER, "hal: using firmware dtb\n");
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
		hal_consolePrint(ATTR_USER, "hal: using syspage dtb\n");
	}

	_pmap_preinit(dtbStart, dtbEnd);
	_hal_platformInit();
	_hal_consoleInit();
	hal_consolePrint(ATTR_USER, "hal: console init done\n");
	_hal_exceptionsInit();
	_hal_interruptsInit();
	_hal_cpuInit();
	_hal_timerInit(SYSTICK_INTERVAL);

	return;
}
