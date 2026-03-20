/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * ARM architectural timer helpers
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_AARCH64_GTIMER_H_
#define _PH_HAL_AARCH64_GTIMER_H_

#include "dtb.h"


const char *hal_gtimerName(dtb_timerSource_t source);


u64 hal_gtimerGetCount(dtb_timerSource_t source);


u32 hal_gtimerGetControl(dtb_timerSource_t source);


void hal_gtimerSetControl(dtb_timerSource_t source, u32 val);


void hal_gtimerSetTimer(dtb_timerSource_t source, u32 ticks);


#endif
