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

	/* TD-04-hack-2: localization probes inside _hal_init. These are
	 * TD-05-class diagnostic markers, but they also appear to act as
	 * Heisenbug insurance — without them the kernel hangs at slightly
	 * different points depending on code layout. To be stripped or
	 * gated behind a debug flag once the underlying issue is fixed.
	 * Markers go through the same TTBR1-mapped early UART used by
	 * the syspage_init probes. */
	volatile unsigned int *uart = (volatile unsigned int *)0xffffffffffe00000ull;

	*uart = 'H'; /* H marker - _hal_init entry */

	hal_common.started = 0;
	*uart = '4';

	schedulerLocked = 0;
	*uart = '5';

	_hal_spinlockInit();
	*uart = '6';

	if ((hal_syspage->hs.firmwareDtb != 0u) && (hal_syspage->hs.firmwareDtbSize != 0u)) {
		*uart = 'F'; /* F marker - using firmware dtb */
		dtbStart = hal_syspage->hs.firmwareDtb;
		dtbEnd = dtbStart + hal_syspage->hs.firmwareDtbSize;
		hal_consolePrint(ATTR_USER, "hal: using firmware dtb\n");
	}
	else {
		*uart = 'S'; /* S marker - using syspage dtb */
		dtb = syspage_progNameResolve("system.dtb");
		*uart = 'r'; /* r marker - progNameResolve returned */
		if (dtb == NULL) {
			*uart = '!'; /* ! marker - DTB missing, halt */
#ifdef NDEBUG
			hal_cpuReboot();
#else
			for (;;) {
				hal_cpuHalt();
			}
#endif
		}
		*uart = 'D'; /* D marker - dtb non-NULL */

		dtbStart = dtb->start;
		*uart = 's'; /* s marker - after reading dtb->start */

		/* TODO(TD-04-hack-3): dtb->end read hangs the kernel on real
		 * Pi 4 (visible: trace stops at 's' marker; never reaches the
		 * 'E' marker that the original `dtbEnd = dtb->end;` produces).
		 * Same access pattern as the dtb->start read just above —
		 * which DOES work — only the offset differs (24 vs 16). This
		 * is a Heisenbug-class follow-on to TD-04. Faking dtbEnd as a
		 * generous size cap from dtbStart lets boot proceed; the real
		 * DTB header self-describes its size, so _pmap_preinit / DTB
		 * parser will work off the in-DTB length anyway. To be
		 * properly fixed once the underlying issue is rooted out.
		 * See TEMPORARY-FIXES TD-04-hack-3. */
		dtbEnd = dtbStart + 0x10000; /* HACK: 64 KiB upper bound, real size from DTB header */
		*uart = 'E'; /* E marker - dtbEnd faked from dtbStart */
		hal_consolePrint(ATTR_USER, "hal: using syspage dtb\n");
	}

	*uart = '7';
	_pmap_preinit(dtbStart, dtbEnd);
	*uart = '8';

	_hal_platformInit();
	*uart = '9';

	_hal_consoleInit();
	*uart = 'a';

	hal_consolePrint(ATTR_USER, "hal: console init done\n");

	_hal_exceptionsInit();
	*uart = 'b';

	_hal_interruptsInit();
	*uart = 'c';

	_hal_cpuInit();
	*uart = 'd';

	_hal_timerInit(SYSTICK_INTERVAL);
	*uart = 'e';

	return;
}
