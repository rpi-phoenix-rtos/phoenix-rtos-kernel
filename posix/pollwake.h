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

#ifndef _PH_POSIX_POLLWAKE_H_
#define _PH_POSIX_POLLWAKE_H_

#include "hal/hal.h"
#include "include/types.h"
#include "proc/proc.h"


/*
 * A descriptor served by another process (a device, a FIFO, an inet socket)
 * reports its readiness only when asked: posix_poll() sends it an atPollStatus
 * query. Without a way back, a poller whose query found nothing ready can only
 * sleep and ask again, which is what the POLL_INTERVAL loop does.
 *
 * pollwake is that way back. A poller that is about to sleep puts a waiter on
 * its own kernel stack and lists it here, together with a 64-bit mask of the
 * oids it asked about (one bit per hash bucket of (port, id)). A server whose
 * oid becomes ready calls the pollNotify() syscall with that oid, and every
 * waiter whose mask has the oid's bit is woken.
 *
 * Nothing is lost between the query and the sleep: the waiter is listed, with
 * its bit set, BEFORE the query is sent, and it sleeps on a queue of its own.
 * A notify that arrives while the poller is still busy therefore leaves
 * wakeupPending in that queue, and the sleep that follows returns at once.
 *
 * An anonymous pipe lives in the kernel but uses the same mechanism: its oid
 * names no server, and its channel calls pollwake_notify() on every state
 * change (posix/pipe.h).
 *
 * Servers that never call pollNotify() are not affected: the poller still
 * sleeps at most POLL_INTERVAL and asks again, exactly as before. A hash
 * collision or an unrelated notify only costs one extra round of queries.
 */
typedef struct _pollwake_waiter_t {
	struct _pollwake_waiter_t *next;
	struct _pollwake_waiter_t *prev;
	thread_t *queue; /* only the owning thread ever sleeps here */
	u64 mask;        /* hash buckets of the watched oids */
	u8 watchUnix;    /* the set holds an AF_UNIX socket */
} pollwake_waiter_t;


void pollwake_init(void);


/* Prepares an unlisted waiter with an empty mask. */
void pollwake_waiterInit(pollwake_waiter_t *w);


/* Adds an oid to the waiter's mask. Call it before the oid is queried. */
void pollwake_watch(pollwake_waiter_t *w, const oid_t *oid);


void pollwake_register(pollwake_waiter_t *w);


void pollwake_unregister(pollwake_waiter_t *w);


/* Sleeps until a watched oid is notified or the absolute deadline (proc_gettime
 * units) passes. Returns -EINTR if a caught signal interrupted the sleep. */
int pollwake_wait(pollwake_waiter_t *w, time_t deadline);


/* Wakes the waiters that watch oid. */
void pollwake_notify(const oid_t *oid);


/* Wakes the waiters whose set holds an AF_UNIX socket (see uchannel_pollNotify). */
void pollwake_notifyUnix(void);

#endif
