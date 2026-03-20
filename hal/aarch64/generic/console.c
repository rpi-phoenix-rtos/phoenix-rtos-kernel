/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * HAL console stub for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/console.h"


void hal_consolePrint(int attr, const char *s)
{
	(void)attr;
	(void)s;
}


void hal_consolePutch(char c)
{
	(void)c;
}


__attribute__((section(".init"))) void _hal_consoleInit(void)
{
}
