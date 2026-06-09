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
	enum { pctl_reboot = 0, pctl_graphmode, pctl_watchpoint } type;

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

		/* Self-hosted A72 data watchpoint (debug/diagnostic, Route A). Arms a
		 * store watchpoint on `va` (EL0+EL1); the watchpoint debug exception is
		 * dumped over the console by exceptions_watchpointHandler. Used to catch
		 * the writer of a corrupted location (e.g. USB #121 hub_common.events).
		 * enable==0 disarms. Single comparator (DBGWVR0/DBGWCR0). */
		struct {
			unsigned long va;
			unsigned int enable;
		} watchpoint;
	} task;
} __attribute__((packed)) platformctl_t;

/* clang-format on */

#endif
