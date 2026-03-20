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

#define ASID_BITS       16U
#define NUM_CPUS        1U
#define SIZE_INTERRUPTS 256U

#ifndef __ASSEMBLY__

#define HAL_NAME_PLATFORM "AArch64 Generic "

#include "include/arch/aarch64/generic/generic.h"
#include "include/arch/aarch64/generic/syspage.h"
#include "include/syspage.h"

#endif

#endif
