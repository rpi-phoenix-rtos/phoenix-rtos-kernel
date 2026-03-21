/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Generic AArch64 basic peripherals control functions
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_ARCH_AARCH64_GENERIC_H_
#define _PH_ARCH_AARCH64_GENERIC_H_

#define PCTL_REBOOT_MAGIC 0xaa55aa55UL

/* clang-format off */

typedef struct {
	enum { pctl_set = 0, pctl_get } action;
	enum { pctl_reboot = 0, pctl_graphmode } type;

	union {
		struct {
			unsigned int magic;
			unsigned int reason;
		} reboot;

		struct {
			unsigned short width;
			unsigned short height;
			unsigned short bpp;
			unsigned short pitch;
			unsigned long framebuffer; /* addr_t */
		} graphmode;
	} task;
} __attribute__((packed)) platformctl_t;

/* clang-format on */

#endif
