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

#include "gtimer.h"
#include "aarch64.h"


const char *hal_gtimerName(dtb_timerSource_t source)
{
	if (source == dtb_timerPhysNonSecure) {
		return "physical-nonsecure";
	}

	if (source == dtb_timerVirt) {
		return "virtual";
	}

	return "unknown";
}


u64 hal_gtimerGetCount(dtb_timerSource_t source)
{
	if (source == dtb_timerVirt) {
		return hal_gtimerGetVirtualCount();
	}

	return hal_gtimerGetPhysicalCount();
}


u32 hal_gtimerGetControl(dtb_timerSource_t source)
{
	if (source == dtb_timerVirt) {
		return hal_gtimerGetVirtualControl();
	}

	return hal_gtimerGetPhysicalControl();
}


void hal_gtimerSetControl(dtb_timerSource_t source, u32 val)
{
	if (source == dtb_timerVirt) {
		hal_gtimerSetVirtualControl(val);
		return;
	}

	hal_gtimerSetPhysicalControl(val);
}


void hal_gtimerSetTimer(dtb_timerSource_t source, u32 ticks)
{
	if (source == dtb_timerVirt) {
		hal_gtimerSetVirtualTimer(ticks);
		return;
	}

	hal_gtimerSetPhysicalTimer(ticks);
}
