/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * PL011 helper
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_AARCH64_PL011_H_
#define _PH_HAL_AARCH64_PL011_H_

#include "hal/types.h"


typedef struct {
	volatile u32 *base;
	u32 stalled;          /* the last wait for room in the TX FIFO timed out */
	volatile u32 dropped; /* characters dropped because the UART did not drain */
} hal_pl011_t;


int hal_pl011Init(hal_pl011_t *uart, addr_t base);


/*
 * Writes c, waiting a bounded time for room in the TX FIFO, so a UART that
 * stops transmitting cannot hang the CPU (and every CPU waiting for the
 * console lock behind it). After one timeout the UART counts as stalled: each
 * further character checks the FIFO once and is dropped if there is still no
 * room, until a character fits again. Returns 0, or -1 if c was dropped.
 */
int hal_pl011Putch(hal_pl011_t *uart, char c);


/* Waits, for a bounded time, until the UART has sent everything */
void hal_pl011Flush(hal_pl011_t *uart);


#endif
