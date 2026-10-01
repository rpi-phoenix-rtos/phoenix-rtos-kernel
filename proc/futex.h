/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Futexes: sleeping on a word of user memory
 *
 * Copyright 2026 Phoenix Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_PROC_FUTEX_H_
#define _PH_PROC_FUTEX_H_

#include "hal/hal.h"


/* Sleeps while *uaddr == val. `timeout` and `clock` as in proc_mutexLock()
 * (0 = no timeout). Returns EOK when woken (or spuriously), -EAGAIN if *uaddr
 * did not hold val, -ETIME, -EINTR, or -EINVAL/-EFAULT for a bad address. */
int proc_futexWait(u32 *uaddr, u32 val, time_t timeout, int clock);


/* Wakes up to `count` threads sleeping on uaddr; returns how many it woke. */
int proc_futexWake(u32 *uaddr, u32 count);


void _futex_init(void);


#endif
