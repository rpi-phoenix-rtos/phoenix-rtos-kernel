/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Futexes: sleeping on a word of user memory
 *
 * The slow path of libphoenix's user-space mutexes and condition variables.
 * A lock is taken and released with an atomic operation on a word in the
 * process's own memory; the kernel is entered only to sleep while the word
 * holds an expected value (futexWait) and to wake such sleepers (futexWake).
 * The kernel keeps no state for a word nobody sleeps on.
 *
 * A word is identified by the address map it lives in and its address. A vfork()
 * child borrows its parent's map, so it and the parent's other threads agree on
 * every word -- they share the memory, and must share the sleepers too.
 *
 * Sleepers are kept in a fixed table of buckets, each guarded by a kernel mutex
 * (not a spinlock: reading the user word can fault, and faults may sleep). The
 * value check and the enqueue happen under the bucket mutex, and so does every
 * wake-up, so a waker that changes the word and then calls futexWake cannot
 * miss a sleeper that saw the old value.
 *
 * Copyright 2026 Phoenix Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "include/errno.h"
#include "lib/lib.h"
#include "vm/vm.h"
#include "threads.h"
#include "process.h"
#include "lock.h"
#include "futex.h"


#define FUTEX_BUCKETS 64U


/* The sleepers on one word; exists only while someone sleeps on it */
typedef struct _futex_waitq_t {
	struct _futex_waitq_t *next, *prev;
	const vm_map_t *map;
	ptr_t addr;
	thread_t *queue;
	unsigned int waiters; /* threads inside proc_futexWait() for this word */
} futex_waitq_t;


typedef struct {
	lock_t lock;
	futex_waitq_t *waitqs;
} futex_bucket_t;


static struct {
	futex_bucket_t buckets[FUTEX_BUCKETS];
} futex_common;


static futex_bucket_t *futex_bucket(const vm_map_t *map, ptr_t addr)
{
	ptr_t h = (addr >> 2) ^ ((ptr_t)map >> 6);

	h ^= h >> 7;

	return &futex_common.buckets[h % FUTEX_BUCKETS];
}


/* Caller holds b->lock */
static futex_waitq_t *_futex_find(futex_bucket_t *b, const vm_map_t *map, ptr_t addr)
{
	futex_waitq_t *wq = b->waitqs;

	if (wq != NULL) {
		do {
			if ((wq->map == map) && (wq->addr == addr)) {
				return wq;
			}
			wq = wq->next;
		} while (wq != b->waitqs);
	}

	return NULL;
}


static int futex_checkAddr(const process_t *process, const u32 *uaddr)
{
	if ((process == NULL) || (((ptr_t)uaddr & (sizeof(*uaddr) - 1U)) != 0U)) {
		return -EINVAL;
	}

	return EOK;
}


int proc_futexWait(u32 *uaddr, u32 val, time_t timeout, int clock)
{
	process_t *process = proc_current()->process;
	const vm_map_t *map;
	futex_bucket_t *b;
	futex_waitq_t *wq;
	time_t abstime;
	int err;

	err = futex_checkAddr(process, uaddr);
	if (err < 0) {
		return err;
	}

	if (vm_mapBelongs(process, uaddr, sizeof(*uaddr)) < 0) {
		return -EFAULT;
	}

	err = proc_clockTimeoutToAbsTime(clock, timeout, &abstime);
	if (err < 0) {
		return err;
	}

	map = process->mapp;
	b = futex_bucket(map, (ptr_t)uaddr);

	(void)proc_lockSet(&b->lock);

	/* The waker changes the word BEFORE it takes the bucket lock, so this read
	 * either sees the new value or the waker has yet to look for sleepers. */
	if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
		(void)proc_lockClear(&b->lock);
		return -EAGAIN;
	}

	wq = _futex_find(b, map, (ptr_t)uaddr);
	if (wq == NULL) {
		wq = vm_kmalloc(sizeof(*wq));
		if (wq == NULL) {
			(void)proc_lockClear(&b->lock);
			return -ENOMEM;
		}
		wq->map = map;
		wq->addr = (ptr_t)uaddr;
		wq->queue = NULL;
		wq->waiters = 0;
		LIST_ADD(&b->waitqs, wq);
	}

	wq->waiters++;

	/* Releases b->lock and sleeps atomically; returns with b->lock held again,
	 * except on -EINTR. */
	err = proc_lockWait(&wq->queue, &b->lock, abstime);
	if (err == -EINTR) {
		(void)proc_lockSet(&b->lock);
	}

	wq->waiters--;
	if (wq->waiters == 0U) {
		/* Also drops a wake-up that found nobody asleep (see proc_futexWake) */
		LIST_REMOVE(&b->waitqs, wq);
		vm_kfree(wq);
	}

	(void)proc_lockClear(&b->lock);

	return err;
}


int proc_futexWake(u32 *uaddr, u32 count)
{
	process_t *process = proc_current()->process;
	futex_bucket_t *b;
	futex_waitq_t *wq;
	u32 woken = 0;
	int err;

	err = futex_checkAddr(process, uaddr);
	if (err < 0) {
		return err;
	}

	b = futex_bucket(process->mapp, (ptr_t)uaddr);

	(void)proc_lockSet(&b->lock);

	wq = _futex_find(b, process->mapp, (ptr_t)uaddr);
	if (wq != NULL) {
		/* A sleeper can leave the queue on its own (timeout, signal) without the
		 * bucket lock, so the queue may be empty although waiters != 0. Waking an
		 * empty queue leaves a pending wake-up in it; the next sleeper on this
		 * word then returns at once, which the user-space loops tolerate, and
		 * the queue is freed with its last waiter anyway. */
		while ((woken < count) && (__atomic_load_n(&wq->queue, __ATOMIC_RELAXED) != NULL) &&
				(proc_threadWakeup(&wq->queue) != 0)) {
			woken++;
		}
	}

	(void)proc_lockClear(&b->lock);

	return (int)woken;
}


void _futex_init(void)
{
	unsigned int i;

	for (i = 0; i < FUTEX_BUCKETS; i++) {
		(void)proc_lockInit(&futex_common.buckets[i].lock, &proc_lockAttrDefault, "futex.bucket");
		futex_common.buckets[i].waitqs = NULL;
	}
}
