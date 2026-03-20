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
#define NUM_CPUS        1U
#define SIZE_INTERRUPTS 256U

#ifndef PL011_TTY_BASE
#define PL011_TTY_BASE 0x09000000u
#endif

#ifndef TIMER_IRQ_GROUP
#define TIMER_IRQ_GROUP 1U
#endif

#ifndef ARM_LOCAL_BASE
#define ARM_LOCAL_BASE 0U
#endif

#define ARM_LOCAL_TIMER_INT_CONTROL0_OFFSET 0x040U
#define ARM_LOCAL_IRQ_PENDING0_OFFSET      0x060U
#define ARM_LOCAL_IRQ_CNTPNS               (1U << 1)

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
