/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Readiness wake-up for poll() on server-backed descriptors
 *
 * Copyright 2026 Phoenix Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "include/errno.h"
#include "lib/lib.h"
#include "proc/proc.h"

#include "pollwake.h"


/*
 * The list lock is a private spinlock. pollwake_notifyUnix() runs inside a held
 * channel mutex (uchannel_pollNotify) and pollwake_wait() hands the lock to
 * proc_threadWaitInterruptible(), which takes threads_common.spinlock under it:
 * the order is always this lock, then the scheduler's.
 */
static struct {
	spinlock_t lock;
	pollwake_waiter_t *waiters;
} pollwake_common;


/* 64 buckets of (port, id). Only 32-bit arithmetic, so no target needs a
 * 64-bit multiply or divide helper. */
static u64 pollwake_bit(const oid_t *oid)
{
	u32 h = (oid->port * 0x9e3779b1U) ^ (u32)oid->id ^ (u32)((u64)oid->id >> 32);

	h *= 0x9e3779b1U;

	return (u64)1 << (h >> 26);
}


void pollwake_init(void)
{
	hal_spinlockCreate(&pollwake_common.lock, "pollwake");
	pollwake_common.waiters = NULL;
}


void pollwake_waiterInit(pollwake_waiter_t *w)
{
	w->next = NULL;
	w->prev = NULL;
	w->queue = NULL;
	w->mask = 0;
	w->watchUnix = 0;
}


void pollwake_watch(pollwake_waiter_t *w, const oid_t *oid)
{
	spinlock_ctx_t sc;
	u64 bit = pollwake_bit(oid);

	/* Only the owner writes the mask, so reading it unlocked here is safe; and
	 * until the waiter is listed (next != NULL) nobody else reads it either. */
	if (w->next == NULL) {
		w->mask |= bit;
	}
	else if ((w->mask & bit) == 0U) {
		hal_spinlockSet(&pollwake_common.lock, &sc);
		w->mask |= bit;
		hal_spinlockClear(&pollwake_common.lock, &sc);
	}
}


void pollwake_register(pollwake_waiter_t *w)
{
	spinlock_ctx_t sc;

	hal_spinlockSet(&pollwake_common.lock, &sc);
	LIST_ADD(&pollwake_common.waiters, w);
	hal_spinlockClear(&pollwake_common.lock, &sc);
}


void pollwake_unregister(pollwake_waiter_t *w)
{
	spinlock_ctx_t sc;

	hal_spinlockSet(&pollwake_common.lock, &sc);
	LIST_REMOVE(&pollwake_common.waiters, w);
	hal_spinlockClear(&pollwake_common.lock, &sc);
}


int pollwake_wait(pollwake_waiter_t *w, time_t deadline)
{
	spinlock_ctx_t sc;
	int err;

	/* Always interruptible. A caught signal ends poll() with -EINTR (POSIX), as
	 * it did before this wait existed: sets with an AF_UNIX socket blocked in an
	 * interruptible wait on a queue shared by all of them, and sets of server fds
	 * only got it from the next re-query's interruptible proc_send(), up to 20 ms
	 * later. An uninterruptible wait here kept that delay (measured 1001.4 ms vs
	 * 1000.0 ms for the mixed set) and also delayed a thread kill by up to 20 ms. */
	hal_spinlockSet(&pollwake_common.lock, &sc);
	err = proc_threadWaitInterruptible(&w->queue, &pollwake_common.lock, deadline, &sc);
	hal_spinlockClear(&pollwake_common.lock, &sc);

	return err;
}


void pollwake_notify(const oid_t *oid)
{
	spinlock_ctx_t sc;
	pollwake_waiter_t *w;
	u64 bit = pollwake_bit(oid);

	hal_spinlockSet(&pollwake_common.lock, &sc);
	w = pollwake_common.waiters;
	if (w != NULL) {
		do {
			if ((w->mask & bit) != 0U) {
				/* Leaves wakeupPending if the owner is not asleep yet. */
				(void)proc_threadWakeup(&w->queue);
			}
			w = w->next;
		} while (w != pollwake_common.waiters);
	}
	hal_spinlockClear(&pollwake_common.lock, &sc);
}


void pollwake_notifyUnix(void)
{
	spinlock_ctx_t sc;
	pollwake_waiter_t *w;

	hal_spinlockSet(&pollwake_common.lock, &sc);
	w = pollwake_common.waiters;
	if (w != NULL) {
		do {
			if (w->watchUnix != 0U) {
				(void)proc_threadWakeup(&w->queue);
			}
			w = w->next;
		} while (w != pollwake_common.waiters);
	}
	hal_spinlockClear(&pollwake_common.lock, &sc);
}
