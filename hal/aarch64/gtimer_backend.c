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

#include "gtimer_backend.h"
#include "aarch64.h"
#include "gtimer.h"

#include "include/errno.h"


int hal_gtimerInitState(hal_gtimerState_t *state)
{
	int err, intr;

	if (state == NULL) {
		return -EINVAL;
	}

	state->source = dtb_timerNone;
	state->irq = 0U;
	state->frequency = 0U;

	intr = -1;
	err = dtb_getTimerSource(&state->source, &intr);
	if (err < 0) {
		return err;
	}

	state->irq = (unsigned int)intr;
	state->frequency = hal_gtimerGetFrequency();

	return EOK;
}


u64 hal_gtimerStateGetCount(const hal_gtimerState_t *state)
{
	if (state == NULL) {
		return 0U;
	}

	return hal_gtimerGetCount(state->source);
}


time_t hal_gtimerStateCyc2us(const hal_gtimerState_t *state, u64 cycles)
{
	if ((state == NULL) || (state->frequency == 0U)) {
		return 0;
	}

	return (time_t)((cycles * 1000000ULL) / state->frequency);
}


time_t hal_gtimerStateGetUs(const hal_gtimerState_t *state)
{
	return hal_gtimerStateCyc2us(state, hal_gtimerStateGetCount(state));
}


u32 hal_gtimerStateUs2Ticks(const hal_gtimerState_t *state, time_t us)
{
	u64 ticks;

	if ((state == NULL) || (state->frequency == 0U) || (us <= 0)) {
		return 0U;
	}

	ticks = ((u64)us * state->frequency) / 1000000ULL;
	if (ticks > 0xffffffffULL) {
		return 0xffffffffU;
	}

	return (u32)ticks;
}


u32 hal_gtimerStateGetControl(const hal_gtimerState_t *state)
{
	if (state == NULL) {
		return 0U;
	}

	return hal_gtimerGetControl(state->source);
}


void hal_gtimerStateSetControl(const hal_gtimerState_t *state, u32 val)
{
	if (state == NULL) {
		return;
	}

	hal_gtimerSetControl(state->source, val);
}


void hal_gtimerStateSetTimer(const hal_gtimerState_t *state, u32 ticks)
{
	if (state == NULL) {
		return;
	}

	hal_gtimerSetTimer(state->source, ticks);
}
