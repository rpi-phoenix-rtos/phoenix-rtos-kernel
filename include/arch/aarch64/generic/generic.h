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
	enum { pctl_reboot = 0 } type;

	union {
		struct {
			unsigned int magic;
			unsigned int reason;
		} reboot;
	} task;
} __attribute__((packed)) platformctl_t;

/* clang-format on */

#endif
