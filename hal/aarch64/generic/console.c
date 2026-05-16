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
