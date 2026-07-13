/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Configuration file for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_CONFIG_H_
#define _PH_HAL_CONFIG_H_

#include <board_config.h>

#define ASID_BITS       16U
/* Pi 4 / BCM2711 has 4 Cortex-A72 cores and Phoenix-RTOS runs full 4-core SMP.
 * NUM_CPUS is 4 and all four A72 cores schedule: the firmware releases the
 * secondaries into the armstub spin-table (confirmed reliable, 2026-05-24), each
 * secondary passes the hal_smpPrimaryReady gate in _other_core_virtual, brings up
 * its per-CPU state (_set_up_vbar_and_stacks, _hal_interruptsInitPerCPU,
 * _hal_cpuInit, _hal_timerInitPerCPU), unmasks DAIF and takes its own timer PPI,
 * so it runs the scheduler (global run-queue, per-core CNTV preemption) alongside
 * cpu0. Load distributes across all four cores (phase-E saturation validation
 * 2026-05-25; HW-reconfirmed 2026-07-14 with cpuburn+top per-core).
 *
 * Residual caveat: there is no PSCI smc / memory-poke fallback that force-releases
 * a secondary, so a core the firmware never released into the armstub on a given
 * cold boot stays down (cpu0 always runs). Not observed since the 2026-05-24
 * firmware-release confirmation, but the recovery path is unimplemented. */
#define NUM_CPUS        4U
#define SIZE_INTERRUPTS 256U

#ifndef PL011_TTY_BASE
#define PL011_TTY_BASE 0x09000000u
#endif

#ifndef TIMER_IRQ_GROUP
#define TIMER_IRQ_GROUP 1U
#endif

#ifndef __ASSEMBLY__

#if defined(__TARGET_AARCH64A72)
#define HAL_NAME_PLATFORM "AArch64 Cortex-A72 Generic "
#else
#define HAL_NAME_PLATFORM "AArch64 Cortex-A53 Generic "
#endif

#include "include/arch/aarch64/generic/generic.h"
#include "include/arch/aarch64/generic/syspage.h"
#include "include/syspage.h"

#endif

#endif
