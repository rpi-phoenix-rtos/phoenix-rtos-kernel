/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * HAL console (via PL011)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/console.h"
#include "hal/hal.h"
#include "hal/spinlock.h"

#include "hal/aarch64/dtb.h"
#include "hal/aarch64/heartbeat.h"
#include "hal/aarch64/pl011.h"

#ifdef KERNEL_HEARTBEAT
#include "hal/timer.h"
#include "lib/lib.h"
#endif


/* The early-console alias, see _hal_consoleEarlyPutch() */
#define CONSOLE_EARLY_UART ((volatile u32 *)0xffffffffffe00000ull)


static struct {
	hal_pl011_t early;
	hal_pl011_t uart;
	spinlock_t lock;
	int enabled;
} console_common = {
	.early = { .base = CONSOLE_EARLY_UART },
};


/* CROSS-BOARD PORTABILITY NOTE (upstream review item "B5"): the early-console
 * path below hardcodes the UART at the fixed VA alias 0xffffffffffe00000,
 * because it must print before hal_consoleInit() has discovered the real base
 * from the DTB. On the Raspberry Pi 4B this alias IS the DTB-discovered pl011
 * base, so it is correct here and intentionally left as-is. But when porting to
 * ANOTHER aarch64 board (Pi 5, Pi Zero 2 W, other BCM/non-BCM SoCs) whose
 * console is not mapped at this alias, this early-print path must be made to use
 * the discovered/board-specific base (conditionalized on early-vs-initialized),
 * or early boot output will go to the wrong address. See docs/KNOWN-ISSUES.md
 * ("Early-console alias — cross-board port"). Do not "fix" it for the Pi 4. */
static void _hal_consoleEarlyPutch(char c)
{
	/* Bounded: if the UART behind the alias stops draining, output is dropped
	 * instead of this CPU spinning on the status register forever, with the
	 * console lock held and every other printing CPU spinning behind it */
	(void)hal_pl011Putch(&console_common.early, c);
}


static void _hal_consoleEarlyPrint(const char *s)
{
	for (; *s != '\0'; ++s) {
		_hal_consoleEarlyPutch(*s);
	}
}


static void _hal_consoleProbe(hal_pl011_t *uart, const char *s)
{
	for (; *s != '\0'; ++s) {
		hal_pl011Putch(uart, *s);
	}
}


void hal_consolePrint(int attr, const char *s)
{
	spinlock_ctx_t sc;
	int locked = 0;

	/* Serialize the whole (attr + string + reset) sequence so concurrent
	 * writers — other CPUs, and every userspace debug() syscall, which all
	 * land here (syscalls_debug -> hal_consolePrint) — cannot interleave
	 * mid-line. Before the console lock exists (pre-_hal_consoleInit) or
	 * before SMP is up (hal_started()==0) writers are single-core, so no
	 * lock is taken. The lock is shared with hal_consolePutch (the kernel
	 * klog mirror), so the two console paths are mutually exclusive. It is a
	 * leaf lock: hal_consolePrint never takes log_common.lock, and the only
	 * nesting (log_write -> mirror -> hal_consolePutch) is always
	 * log_common.lock -> console_common.lock, so no deadlock. */
	if ((console_common.enabled != 0) && (hal_started() != 0)) {
		hal_spinlockSet(&console_common.lock, &sc);
		locked = 1;
	}

	if (attr == ATTR_BOLD) {
		_hal_consoleEarlyPrint(CONSOLE_BOLD);
	}
	else if (attr != ATTR_USER) {
		_hal_consoleEarlyPrint(CONSOLE_CYAN);
	}
	else {
		/* No action required */
	}

	_hal_consoleEarlyPrint(s);
	_hal_consoleEarlyPrint(CONSOLE_NORMAL);

	if (locked != 0) {
		hal_spinlockClear(&console_common.lock, &sc);
	}
}


void hal_consolePutch(char c)
{
	spinlock_ctx_t sc;

	if (console_common.enabled == 0) {
		return;
	}

	if (hal_started() == 0) {
		hal_pl011Putch(&console_common.uart, c);
		return;
	}

	hal_spinlockSet(&console_common.lock, &sc);
	hal_pl011Putch(&console_common.uart, c);
	hal_spinlockClear(&console_common.lock, &sc);
}


#ifdef KERNEL_HEARTBEAT

#ifndef KERNEL_HEARTBEAT_SEC
#define KERNEL_HEARTBEAT_SEC 10
#endif

#define HEARTBEAT_PERIOD_US ((time_t)KERNEL_HEARTBEAT_SEC * 1000000)


/* Each CPU writes only its own entry, from its own timer interrupt. Every
 * CPU prints, at its own time slot, the tick counts of all CPUs: a CPU that
 * stopped taking interrupts shows as a count that no longer grows, in the
 * lines of the CPUs that still run. */
static struct {
	volatile u32 ticks;
	volatile u64 pc;     /* where the last timer interrupt found the CPU */
	volatile u32 el;
	time_t next;         /* when this CPU prints next */
	u32 seen[NUM_CPUS];  /* tick counts at this CPU's previous line */
} __attribute__((aligned(64))) heartbeat_cpu[NUM_CPUS];


static void _hal_heartbeatPrint(unsigned int id, time_t now)
{
	char line[64 + NUM_CPUS * 40];
	char *p = line;
	unsigned int i, stopped = 0U;
	u32 ticks;

	p += lib_sprintf(p, "hb cpu%u t=%u ticks", id, (u32)(now / 1000000));
	for (i = 0U; i < NUM_CPUS; i++) {
		ticks = heartbeat_cpu[i].ticks;
		if (ticks == heartbeat_cpu[id].seen[i]) {
			stopped |= 1U << i;
		}
		heartbeat_cpu[id].seen[i] = ticks;
		p += lib_sprintf(p, " %u%s", ticks, ((stopped & (1U << i)) != 0U) ? "!" : "");
	}

	/* Where this CPU, and each CPU whose count stopped, was last interrupted */
	for (i = 0U; i < NUM_CPUS; i++) {
		if ((i == id) || ((stopped & (1U << i)) != 0U)) {
			p += lib_sprintf(p, " %u@%llx/el%u", i, (u64)heartbeat_cpu[i].pc, heartbeat_cpu[i].el);
		}
	}
	(void)lib_sprintf(p, " drop %u\n", console_common.early.dropped + console_common.uart.dropped);

	/* Past the console lock: the CPU holding it may be the one that hangs */
	for (p = line; *p != '\0'; p++) {
		(void)hal_pl011Putch(&console_common.early, *p);
	}
}


void _hal_heartbeat(const cpu_context_t *ctx)
{
	unsigned int id = hal_cpuGetID();
	time_t now, slot;

	if (id >= NUM_CPUS) {
		return;
	}

	heartbeat_cpu[id].ticks++;
	heartbeat_cpu[id].pc = ctx->pc;
	heartbeat_cpu[id].el = (u32)((ctx->psr >> 2) & 3U);

	now = hal_timerGetUs();
	if (now < heartbeat_cpu[id].next) {
		return;
	}

	if (heartbeat_cpu[id].next != 0) {
		_hal_heartbeatPrint(id, now);
	}

	/* The CPUs take turns, spread over the period, so their lines do not collide */
	slot = (HEARTBEAT_PERIOD_US / (time_t)NUM_CPUS) * (time_t)id;
	heartbeat_cpu[id].next = ((now - slot) / HEARTBEAT_PERIOD_US + 1) * HEARTBEAT_PERIOD_US + slot;
}

#endif /* KERNEL_HEARTBEAT */


__attribute__((section(".init"))) void _hal_consoleInit(void)
{
	dtb_serial_t serial;

	if ((dtb_getConsoleSerial(&serial) < 0) || (hal_pl011Init(&console_common.uart, serial.base) < 0)) {
		console_common.enabled = 0;
		return;
	}

	_hal_consoleProbe(&console_common.uart, "console: pl011 init done\n");

	hal_spinlockCreate(&console_common.lock, "console_common.lock");
	console_common.enabled = 1;
}
