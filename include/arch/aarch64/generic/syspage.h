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
#if defined(HAS_GRAPHICS) && (HAS_GRAPHICS != 0)
	struct {
		unsigned short width;
		unsigned short height;
		unsigned short bpp;
		unsigned short pitch;
		unsigned long framebuffer; /* addr_t */
	} __attribute__((packed)) graphmode;
#endif
	unsigned long firmwareDtb;     /* addr_t */
	unsigned long firmwareDtbSize; /* size_t */
} __attribute__((packed)) hal_syspage_t;

#endif
