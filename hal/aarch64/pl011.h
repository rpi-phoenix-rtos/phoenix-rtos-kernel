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
} hal_pl011_t;


int hal_pl011Init(hal_pl011_t *uart, addr_t base);


void hal_pl011Putch(hal_pl011_t *uart, char c);


void hal_pl011Flush(hal_pl011_t *uart);


#endif
