/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * ARM architectural timer backend state
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_AARCH64_GTIMER_BACKEND_H_
#define _PH_HAL_AARCH64_GTIMER_BACKEND_H_

#include "dtb.h"


typedef struct {
	dtb_timerSource_t source;
	unsigned int irq;
	u32 frequency;
} hal_gtimerState_t;


int hal_gtimerInitState(hal_gtimerState_t *state);


#endif
