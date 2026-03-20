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
