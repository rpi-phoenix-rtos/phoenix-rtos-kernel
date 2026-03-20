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
#include "hal/interrupts.h"


typedef struct {
	dtb_timerSource_t source;
	unsigned int irq;
	u32 frequency;
} hal_gtimerState_t;


int hal_gtimerInitState(hal_gtimerState_t *state);


u64 hal_gtimerStateGetCount(const hal_gtimerState_t *state);


time_t hal_gtimerStateCyc2us(const hal_gtimerState_t *state, u64 cycles);


time_t hal_gtimerStateGetUs(const hal_gtimerState_t *state);


u32 hal_gtimerStateUs2Ticks(const hal_gtimerState_t *state, time_t us);


u32 hal_gtimerStateGetControl(const hal_gtimerState_t *state);


void hal_gtimerStateSetControl(const hal_gtimerState_t *state, u32 val);


void hal_gtimerStateSetTimer(const hal_gtimerState_t *state, u32 ticks);


void hal_gtimerStateSetWakeup(const hal_gtimerState_t *state, u32 waitUs);


unsigned int hal_gtimerStateIrq(const hal_gtimerState_t *state);


int hal_gtimerStateRegisterHandler(const hal_gtimerState_t *state, intrFn_t f, void *data, intr_handler_t *h);


#endif
