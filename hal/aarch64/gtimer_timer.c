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
#include "interrupts_gicv2.h"

#include "hal/console.h"
#include "hal/string.h"
#include "hal/timer.h"

#include "include/errno.h"
#include "lib/lib.h"


static struct {
	hal_gtimerState_t state;
	u32 interval;
	int ready;
	int traceConfig;
	int traceWakeup;
} timer_common;


static void timer_tracePrint(const char *fmt, ...)
{
	char buff[80];
	va_list args;

	va_start(args, fmt);
	(void)lib_vsprintf(buff, fmt, args);
	va_end(args);

	hal_consolePrint(ATTR_USER, buff);
}


static u32 timer_traceGetTimerValue(const hal_gtimerState_t *state)
{
	if ((state != NULL) && (state->source == dtb_timerVirt)) {
		return hal_gtimerGetVirtualTimer();
	}

	return hal_gtimerGetPhysicalTimer();
}


static void timer_traceProbePending(const hal_gtimerState_t *state)
{
	u64 start, now;
	time_t elapsed;
	u32 ahppir, control, pending, privatePending, hppir, value;

	if (state == NULL) {
		return;
	}

	start = hal_gtimerStateGetCount(state);
	pending = interrupts_getPending(hal_gtimerStateIrq(state));
	privatePending = interrupts_getPrivatePending(hal_gtimerStateIrq(state));
	hppir = interrupts_getHighestPending();
	ahppir = interrupts_getAliasedHighestPending();

	do {
		pending |= interrupts_getPending(hal_gtimerStateIrq(state));
		privatePending |= interrupts_getPrivatePending(hal_gtimerStateIrq(state));
		hppir = interrupts_getHighestPending();
		ahppir = interrupts_getAliasedHighestPending();

		now = hal_gtimerStateGetCount(state);
		elapsed = hal_gtimerStateCyc2us(state, now - start);
		if (elapsed >= 2000) {
			break;
		}
	} while (1);

	timer_tracePrint("gtimer: pending %u\n", pending);
	timer_tracePrint("gtimer: ppi pending %u\n", privatePending);
	timer_tracePrint("gtimer: hppir %u\n", hppir);
	timer_tracePrint("gtimer: ahppir %u\n", ahppir);
	control = hal_gtimerStateGetControl(state);
	value = timer_traceGetTimerValue(state);
	timer_tracePrint("gtimer: post %lld us ctl 0x%x tval %u\n", (long long)elapsed, control, value);
}


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

	if (timer_common.traceWakeup == 0) {
		timer_common.traceWakeup = 1;
		timer_tracePrint("gtimer: arm %u us ctl 0x%x tval %u\n", waitUs, hal_gtimerStateGetControl(&timer_common.state), timer_traceGetTimerValue(&timer_common.state));
		timer_traceProbePending(&timer_common.state);
	}
}


int hal_timerRegister(intrFn_t f, void *data, intr_handler_t *h)
{
	int err;

	if (timer_common.ready == 0) {
		return -ENODEV;
	}

	if (timer_common.traceConfig == 0) {
		timer_common.traceConfig = 1;
		timer_tracePrint("gtimer: source %s irq %u\n", hal_gtimerName(timer_common.state.source), hal_gtimerStateIrq(&timer_common.state));
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
	timer_common.traceConfig = 0;
	timer_common.traceWakeup = 0;

	if (hal_gtimerInitState(&timer_common.state) < 0) {
		return;
	}

	timer_common.ready = 1;
	hal_gtimerStateSetControl(&timer_common.state, 0U);
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
