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
/* Pi 4 / BCM2711 Cortex-A72 cluster has 4 cores. Secondaries reach
 * the kernel's `_other_core_virtual` WFI loop via plo's SMP Phase A
 * handoff (board_config PLO_SMP_ENABLE=1). With NUM_CPUS=4 the
 * spinlock paths use real LDAXR/STXR, the GIC distributor mask is
 * 4-bit, and the scheduler iterates over all 4 CPUs. Secondaries
 * still park in WFI until Phase C teaches them to enter the
 * scheduler — but bumping NUM_CPUS first surfaces any latent
 * single-CPU assumptions in the primary boot path. */
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
