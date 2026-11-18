/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Kernel heartbeat (debug option)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_AARCH64_HEARTBEAT_H_
#define _PH_HAL_AARCH64_HEARTBEAT_H_

#include "hal/cpu.h"


/*
 * A periodic line per CPU on the console, to locate a hang: which CPUs still
 * take timer interrupts and where each was last interrupted. Off by default;
 * built in with
 *
 *   KERNEL_DIAG='-DKERNEL_HEARTBEAT [-DKERNEL_HEARTBEAT_SEC=<s>]'
 *
 * on boards whose console defines HAL_CONSOLE_HEARTBEAT. The interrupt
 * dispatcher calls _hal_heartbeat() on every timer interrupt of every CPU,
 * before it takes any lock. Every KERNEL_HEARTBEAT_SEC seconds (default 10)
 * each CPU, in its own slot of the period, writes one line straight to the
 * UART, past the console lock and with a bounded wait:
 *
 *   hb cpu<n> t=<s> ticks <c0> <c1> ... <n>@<pc>/el<e> ... drop <d>
 *
 * <cN> counts the timer interrupts of CPU N; a '!' marks a count that has not
 * changed since this CPU's previous line, and that CPU's last interrupted pc
 * and exception level follow this CPU's own. <d> counts the characters the
 * console dropped while the UART did not drain.
 *
 * A CPU that hangs with interrupts masked shows as a stopped count. A CPU that
 * hangs holding the scheduler lock (or the timer's interrupt lock) stops the
 * others at their next tick too, so the last lines are up to a period old:
 * for a hang hunt use a short period, e.g. -DKERNEL_HEARTBEAT_SEC=2.
 */
void _hal_heartbeat(const cpu_context_t *ctx);


#endif
