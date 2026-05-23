/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * ARM architectural timer
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "gtimer_backend.h"
#include "gtimer.h"
#include "aarch64.h"

#include "hal/console.h"
#include "hal/string.h"
#include "hal/timer.h"

#include "include/errno.h"
#include "lib/lib.h"


static struct {
	hal_gtimerState_t state;
	u32 interval;
	int ready;
} timer_common;


time_t hal_timerGetUs(void)
{
	if (timer_common.ready == 0) {
		return 0;
	}

	return hal_gtimerStateGetUs(&timer_common.state);
}


void hal_timerSetWakeup(u32 waitUs)
{
	if (timer_common.ready == 0) {
		return;
	}

	hal_gtimerStateSetWakeup(&timer_common.state, waitUs);
}


int hal_timerRegister(intrFn_t f, void *data, intr_handler_t *h)
{
	int err;

	if (timer_common.ready == 0) {
		return -ENODEV;
	}

	err = hal_gtimerStateRegisterHandler(&timer_common.state, f, data, h);
	if ((err >= 0) && (timer_common.interval != 0U)) {
		hal_gtimerStateSetWakeup(&timer_common.state, timer_common.interval);
	}

	return err;
}


unsigned int hal_timerIrq(void)
{
	if (timer_common.ready == 0) {
		return 0U;
	}

	return hal_gtimerStateIrq(&timer_common.state);
}


void _hal_timerInit(u32 interval)
{
	timer_common.interval = interval;
	timer_common.ready = 0;

	if (hal_gtimerInitState(&timer_common.state) < 0) {
		return;
	}

	timer_common.ready = 1;
	hal_gtimerStateSetControl(&timer_common.state, 0U);
}


/* SMP Phase D observability: per-CPU bring-up counters. Bumped from
 * _hal_timerInitPerCPU below; primary's main_initthr prints them after
 * the spawn loop so we can see whether secondaries reached this code
 * at all. cpu0 stays 0 — primary's _hal_timerInit is the equivalent
 * entry point and it doesn't call this function. */
volatile unsigned int hal_smpTimerInitPerCpuCount[8];

/* SMP Phase D-8 override: when non-zero, secondaries arm CNTV with
 * this interval instead of timer_common.interval for the FIRST
 * tick. Used to defer the first secondary timer PPI past primary's
 * boot-reschedule window — primary's main() sets this before
 * publishing hal_smpPrimaryReady. After the first tick, subsequent
 * arms (in threads_timeintr → hal_timerSetWakeup) use the normal
 * SYSTICK_INTERVAL. */
volatile unsigned int hal_smpFirstIntervalUs = 0U;


/* SMP Phase C step 3: arm this CPU's per-CPU architectural timer so
 * it actually fires the PPI we enabled in _hal_interruptsInitPerCPU.
 * `CNTV_CVAL_EL0` / `CNTV_CTL_EL0` are banked per-CPU, so each
 * secondary needs to program its own — primary's call to
 * hal_timerRegister() only programs CPU 0. Without this the GIC
 * routes the timer PPI but the timer never asserts. */
void _hal_timerInitPerCPU(void)
{
	unsigned int cpuId = hal_cpuGetID();
	u32 interval;

	if (cpuId < (sizeof(hal_smpTimerInitPerCpuCount) / sizeof(hal_smpTimerInitPerCpuCount[0]))) {
		hal_cpuAtomicInc(&hal_smpTimerInitPerCpuCount[cpuId]);
	}

	if (timer_common.ready == 0) {
		return;
	}

	interval = ((cpuId != 0U) && (hal_smpFirstIntervalUs != 0U))
		? hal_smpFirstIntervalUs
		: timer_common.interval;

	hal_gtimerStateSetWakeup(&timer_common.state, interval);
}


char *hal_timerFeatures(char *features, size_t len)
{
	const char *text;

	text = (timer_common.ready != 0) ? "Using ARM architectural timer" : "ARM architectural timer unavailable";
	(void)hal_strncpy(features, text, len);
	if (len != 0U) {
		features[len - 1U] = '\0';
	}

	return features;
}
