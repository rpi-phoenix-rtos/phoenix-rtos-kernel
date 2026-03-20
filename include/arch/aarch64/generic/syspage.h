/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * HAL syspage for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_SYSPAGE_AARCH64_GENERIC_H_
#define _PH_SYSPAGE_AARCH64_GENERIC_H_

typedef struct {
	long long int resetReason;
} __attribute__((packed)) hal_syspage_t;

#endif
