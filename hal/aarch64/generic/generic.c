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
#include "hal/aarch64/dtb.h"
#include "hal/aarch64/arch/pmap.h"

#include "include/errno.h"

#include "hal/cpu.h"
#include "hal/hal.h"


/* BCM2835-family power management block: its watchdog resets the SoC */
#define PM_RSTC            (0x1cU / 4U)
#define PM_RSTS            (0x20U / 4U)
#define PM_WDOG            (0x24U / 4U)
#define PM_PASSWORD        0x5a000000U
#define PM_RSTC_WRCFG_MASK 0x00000030U
#define PM_RSTC_FULL_RESET 0x00000020U
/* The firmware reads the boot partition from the even bits of RSTS; 0 = normal boot */
#define PM_RSTS_PARTITION  0x00000555U
#define PM_WDOG_TICKS      10U /* ~150 us */


static struct {
	spinlock_t lock;
	volatile u32 *pm; /* mapped on first use, NULL until then or if the board has none */
} generic_common;


/* Maps the PM watchdog on first use. Not at _hal_platformInit(): that runs before the console
 * maps the UART, and the first device mapping must be the UART, which reuses the early-UART
 * alias _init.S set up in the first device slot. */
static volatile u32 *generic_pm(void)
{
	addr_t pm;

	if ((generic_common.pm == NULL) && (dtb_getPMWatchdog(&pm) == EOK)) {
		generic_common.pm = _pmap_halMapDevice(pm, 0, SIZE_PAGE);
	}

	return generic_common.pm;
}


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
				/* PM reset status (BCM2835 family: which reset started this boot), 0 if unknown */
				data->task.reboot.reason = (generic_pm() != NULL) ? (generic_pm()[PM_RSTS] & ~PM_PASSWORD) : 0U;
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

		case pctl_watchpoint:
			if (data->action == pctl_set) {
				hal_cpuWatchpointSet(data->task.watchpoint.va, (int)data->task.watchpoint.enable,
						data->task.watchpoint.trapLo, data->task.watchpoint.trapHi);
				ret = 0;
			}
			break;

		case pctl_cpucount:
			if (data->action == pctl_get) {
				data->task.cpucount.count = hal_cpuGetCount();
				ret = 0;
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
	generic_common.pm = NULL;
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
	}
	else {
		hal_cpuAtomicInc(&nCpusStarted);
		hal_cpuDataSyncBarrier();
		hal_cpuSignalEvent();
	}

	/* No cross-CPU rendezvous barrier here on purpose: secondaries gate on
	 * nCpusStarted (released above with a DSB+SEV), so primary must not block. */
}


/* Resets the SoC through the PM watchdog. Returns only if there is no PM watchdog. */
static void generic_pmReset(void)
{
	volatile u32 *pm = generic_pm();

	if (pm == NULL) {
		return;
	}

	pm[PM_RSTS] = PM_PASSWORD | (pm[PM_RSTS] & ~PM_RSTS_PARTITION);

	pm[PM_WDOG] = PM_PASSWORD | PM_WDOG_TICKS;
	pm[PM_RSTC] = PM_PASSWORD | (pm[PM_RSTC] & ~PM_RSTC_WRCFG_MASK) | PM_RSTC_FULL_RESET;
	hal_cpuDataSyncBarrier();
}


__attribute__((noreturn)) void hal_cpuReboot(void)
{
	/* No PSCI SYSTEM_RESET on BCM2711 (no EL3 PSCI monitor); use the PM watchdog where
	 * there is one, otherwise halt in place rather than silently continuing. */
	generic_pmReset();

	for (;;) {
		hal_cpuHalt();
	}
}


void hal_cpuSmpSync(void)
{
	hal_cpuDataSyncBarrier();
	hal_cpuInstrBarrier();
}
