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
#include "hal/spinlock.h"

#include "hal/aarch64/dtb.h"
#include "hal/aarch64/pl011.h"


static struct {
	hal_pl011_t uart;
	spinlock_t lock;
	int enabled;
} console_common;


static void _hal_consolePrint(const char *s)
{
	for (; *s != '\0'; ++s) {
		hal_consolePutch(*s);
	}
}


void hal_consolePrint(int attr, const char *s)
{
	if (attr == ATTR_BOLD) {
		_hal_consolePrint(CONSOLE_BOLD);
	}
	else if (attr != ATTR_USER) {
		_hal_consolePrint(CONSOLE_CYAN);
	}
	else {
		/* No action required */
	}

	_hal_consolePrint(s);
	_hal_consolePrint(CONSOLE_NORMAL);
}


void hal_consolePutch(char c)
{
	spinlock_ctx_t sc;

	if (console_common.enabled == 0) {
		return;
	}

	hal_spinlockSet(&console_common.lock, &sc);
	hal_pl011Putch(&console_common.uart, c);
	hal_spinlockClear(&console_common.lock, &sc);
}


__attribute__((section(".init"))) void _hal_consoleInit(void)
{
	dtb_serial_t *serials;
	size_t nSerials;

	dtb_getSerials(&serials, &nSerials);
	if ((nSerials == 0U) || (hal_pl011Init(&console_common.uart, serials[0].base) < 0)) {
		console_common.enabled = 0;
		return;
	}

	hal_spinlockCreate(&console_common.lock, "console_common.lock");
	console_common.enabled = 1;
}
