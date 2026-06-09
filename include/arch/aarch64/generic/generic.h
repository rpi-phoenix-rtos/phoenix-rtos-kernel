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
			/* If trapHi != 0: the watchpoint halts only when the value being
			 * stored is in [trapLo, trapHi) (e.g. a code-pointer wild write);
			 * legitimate stores of NULL / heap pointers are emulated and
			 * stepped over, keeping the watchpoint armed. If trapHi == 0: halt
			 * on any store (the simple halt-first mode). */
			unsigned long trapLo;
			unsigned long trapHi;
		} watchpoint;
	} task;
} __attribute__((packed)) platformctl_t;

/* clang-format on */

#endif
