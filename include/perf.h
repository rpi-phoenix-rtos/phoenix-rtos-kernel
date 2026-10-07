/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Performance analysis subsystem
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#ifndef _PHOENIX_PERF_H_
#define _PHOENIX_PERF_H_


/* clang-format off */
typedef enum { perf_mode_trace, perf_mode_count } perf_mode_t;
typedef enum { trace_channel_meta, trace_channel_event, trace_channel_count } trace_channel_t;
/* clang-format on */


#define PERF_TRACE_FLAG_ROLLING (1U << 0) /* treat event channel as rolling window */
#define PERF_TRACE_FLAG_SAMPLE  (1U << 1) /* sample the thread running on every CPU (thread_sample events) */


#define PERF_TRACE_DEPTH_MAX  32U   /* longest frame-pointer chain recorded */
#define PERF_TRACE_USTACK_MAX 4096U /* most user stack bytes copied with one event */


#define PERF_TRACE_WAITSTACK_DEFERRED_MAX 1024U /* most waitStack bytes when waitMinUs != 0 */


/* Event classes of perf_trace_cfg_t.events */
#define PERF_TRACE_EV_THREAD  (1U << 0) /* thread_create/end, process_exec/kill, trace_stats */
#define PERF_TRACE_EV_SCHED   (1U << 1) /* scheduling, preempted, enqueued, waking, sched_enter/exit, priority */
#define PERF_TRACE_EV_SYSCALL (1U << 2) /* syscall_enter/exit */
#define PERF_TRACE_EV_LOCK    (1U << 3) /* lock_name, lock_set_*, lock_clear */
#define PERF_TRACE_EV_IRQ     (1U << 4) /* interrupt_enter/exit */
#define PERF_TRACE_EV_WAIT    (1U << 5) /* thread_wait, thread_wakeup */
#define PERF_TRACE_EV_MSG     (1U << 6) /* msg_send/recv/respond */

/* What a profiler needs: who runs (thread_sample, PERF_TRACE_FLAG_SAMPLE), what blocks, who serves */
#define PERF_TRACE_EV_PROFILE (PERF_TRACE_EV_THREAD | PERF_TRACE_EV_WAIT | PERF_TRACE_EV_MSG)


/*
 * Optional argument of perf_start(perf_mode_trace, ...). Without it (arg == NULL) sampling, when
 * requested, uses the defaults below, no user stack is copied and every wait is recorded. Fields
 * are only appended: a shorter sz (an older caller) leaves the fields it lacks 0.
 */
typedef struct {
	unsigned int samplePeriodUs; /* per-CPU sampling period (0: 1000 us); timer tick resolution */
	unsigned int depth;          /* frame-pointer chain length (0: 16, at most PERF_TRACE_DEPTH_MAX) */
	unsigned int sampleStack;    /* user stack bytes above sp copied with a sample (at most PERF_TRACE_USTACK_MAX) */
	unsigned int waitStack;      /* user stack bytes above sp copied when a thread blocks (likewise) */
	/*
	 * 0: thread_wait is written when a wait begins, for every wait. Otherwise only waits that last
	 * at least waitMinUs are written, when they end (or the trace stops), and the wakeups of the
	 * shorter ones are left out: most waits are far shorter than a stall, and this drops them.
	 */
	unsigned int waitMinUs;
	/*
	 * Event classes to record (PERF_TRACE_EV_*), 0: all of them. Scheduling, syscall, lock and
	 * interrupt events come at the rate of those operations, so a profile leaves them out.
	 */
	unsigned int events;
	/* deferred waits (waitMinUs): only waits of at least this long carry a user stack (waitStack) */
	unsigned int waitStackMinUs;
} perf_trace_cfg_t;


#endif
