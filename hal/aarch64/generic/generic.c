/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * HAL internal functions for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/aarch64/aarch64.h"
#include "hal/aarch64/interrupts_gicv2.h"
#include "hal/aarch64/halsyspage.h"

#include "hal/cpu.h"
#include "hal/hal.h"


static struct {
	spinlock_t lock;
} generic_common;


/* parasoft-begin-suppress MISRAC2012-RULE_8_4 "Definition in assembly" */
volatile unsigned int nCpusStarted = 0U;


int _interrupts_gicv2_classify(unsigned int irqn)
{
	return (irqn < 16U) ? gicv2_cfg_reserved : gicv2_cfg_high_level;
}


/* parasoft-end-suppress MISRAC2012-RULE_8_4 */
int hal_platformctl(void *ptr)
{
	platformctl_t *data = ptr;
	spinlock_ctx_t sc;
	int ret = -1;

	hal_spinlockSet(&generic_common.lock, &sc);

	switch (data->type) {
		case pctl_reboot:
			if ((data->action == pctl_set) && (data->task.reboot.magic == PCTL_REBOOT_MAGIC)) {
				hal_cpuReboot();
			}
			else if (data->action == pctl_get) {
				data->task.reboot.reason = 0U;
				ret = 0;
			}
			break;

		case pctl_graphmode:
			if (data->action == pctl_get) {
#if defined(HAS_GRAPHICS) && (HAS_GRAPHICS != 0)
				data->task.graphmode.width = hal_syspage->hs.graphmode.width;
				data->task.graphmode.height = hal_syspage->hs.graphmode.height;
				data->task.graphmode.bpp = hal_syspage->hs.graphmode.bpp;
				data->task.graphmode.pitch = hal_syspage->hs.graphmode.pitch;
				data->task.graphmode.framebuffer = hal_syspage->hs.graphmode.framebuffer;
				ret = 0;
#else
				ret = -1;
#endif
			}
			break;

		default:
			/* No action required */
			break;
	}

	hal_spinlockClear(&generic_common.lock, &sc);

	return ret;
}


void hal_wdgReload(void)
{
}


unsigned int hal_cpuGetCount(void)
{
	return NUM_CPUS;
}


void _hal_platformInit(void)
{
	hal_spinlockCreate(&generic_common.lock, "generic_common.lock");
}


void _hal_cpuInit(void)
{
	if (hal_started() == 0) {
		nCpusStarted++;
		/* Wake any secondary stuck in _other_core_trap's WFE. Primary's
		 * non-atomic store above released the gate that secondaries
		 * poll on, but WFE only wakes on SEV (or a system event such as
		 * an interrupt). Without this, secondaries park forever and
		 * never reach _other_core_virtual on a NUM_CPUS>1 build.
		 * The store is followed by a release barrier so secondaries
		 * observe nCpusStarted!=0 before they see the event. */
		hal_cpuDataSyncBarrier();
		hal_cpuSignalEvent();
		return;
	}

	hal_cpuAtomicInc(&nCpusStarted);
	hal_cpuDataSyncBarrier();
	hal_cpuSignalEvent();
}


__attribute__((noreturn)) void hal_cpuReboot(void)
{
	for (;;) {
		hal_cpuHalt();
	}
}


void hal_cpuSmpSync(void)
{
	hal_cpuDataSyncBarrier();
	hal_cpuInstrBarrier();
}
