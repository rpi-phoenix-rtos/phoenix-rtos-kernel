/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * HAL console (via PL011)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/console.h"
#include "hal/hal.h"
#include "hal/spinlock.h"

#include "hal/aarch64/dtb.h"
#include "hal/aarch64/pl011.h"


static struct {
	hal_pl011_t uart;
	spinlock_t lock;
	int enabled;
} console_common;


/* CROSS-BOARD PORTABILITY NOTE (upstream review item "B5"): the early-console
 * path below hardcodes the UART at the fixed VA alias 0xffffffffffe00000,
 * because it must print before hal_consoleInit() has discovered the real base
 * from the DTB. On the Raspberry Pi 4B this alias IS the DTB-discovered pl011
 * base, so it is correct here and intentionally left as-is. But when porting to
 * ANOTHER aarch64 board (Pi 5, Pi Zero 2 W, other BCM/non-BCM SoCs) whose
 * console is not mapped at this alias, this early-print path must be made to use
 * the discovered/board-specific base (conditionalized on early-vs-initialized),
 * or early boot output will go to the wrong address. See docs/KNOWN-ISSUES.md
 * ("Early-console alias — cross-board port"). Do not "fix" it for the Pi 4. */
static void _hal_consoleEarlyPutch(char c)
{
	volatile u32 *uart = (volatile u32 *)0xffffffffffe00000ull;
	volatile u32 *uartfr = (volatile u32 *)0xffffffffffe00018ull;

	while ((*uartfr & (1U << 5)) != 0U) {
	}

	*uart = (u32)(u8)c;
}


static void _hal_consoleEarlyPrint(const char *s)
{
	for (; *s != '\0'; ++s) {
		_hal_consoleEarlyPutch(*s);
	}
}


static void _hal_consoleProbe(hal_pl011_t *uart, const char *s)
{
	for (; *s != '\0'; ++s) {
		hal_pl011Putch(uart, *s);
	}
}


void hal_consolePrint(int attr, const char *s)
{
	spinlock_ctx_t sc;
	int locked = 0;

	/* Serialize the whole (attr + string + reset) sequence so concurrent
	 * writers — other CPUs, and every userspace debug() syscall, which all
	 * land here (syscalls_debug -> hal_consolePrint) — cannot interleave
	 * mid-line. Before the console lock exists (pre-_hal_consoleInit) or
	 * before SMP is up (hal_started()==0) writers are single-core, so no
	 * lock is taken. The lock is shared with hal_consolePutch (the kernel
	 * klog mirror), so the two console paths are mutually exclusive. It is a
	 * leaf lock: hal_consolePrint never takes log_common.lock, and the only
	 * nesting (log_write -> mirror -> hal_consolePutch) is always
	 * log_common.lock -> console_common.lock, so no deadlock. */
	if ((console_common.enabled != 0) && (hal_started() != 0)) {
		hal_spinlockSet(&console_common.lock, &sc);
		locked = 1;
	}

	if (attr == ATTR_BOLD) {
		_hal_consoleEarlyPrint(CONSOLE_BOLD);
	}
	else if (attr != ATTR_USER) {
		_hal_consoleEarlyPrint(CONSOLE_CYAN);
	}
	else {
		/* No action required */
	}

	_hal_consoleEarlyPrint(s);
	_hal_consoleEarlyPrint(CONSOLE_NORMAL);

	if (locked != 0) {
		hal_spinlockClear(&console_common.lock, &sc);
	}
}


void hal_consolePutch(char c)
{
	spinlock_ctx_t sc;

	if (console_common.enabled == 0) {
		return;
	}

	if (hal_started() == 0) {
		hal_pl011Putch(&console_common.uart, c);
		return;
	}

	hal_spinlockSet(&console_common.lock, &sc);
	hal_pl011Putch(&console_common.uart, c);
	hal_spinlockClear(&console_common.lock, &sc);
}


__attribute__((section(".init"))) void _hal_consoleInit(void)
{
	dtb_serial_t serial;

	if ((dtb_getConsoleSerial(&serial) < 0) || (hal_pl011Init(&console_common.uart, serial.base) < 0)) {
		console_common.enabled = 0;
		return;
	}

	_hal_consoleProbe(&console_common.uart, "console: pl011 init done\n");

	hal_spinlockCreate(&console_common.lock, "console_common.lock");
	console_common.enabled = 1;
}
