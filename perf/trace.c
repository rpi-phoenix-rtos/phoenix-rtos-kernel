/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Performance analysis subsystem - CTF backend
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include "include/perf.h"
#include "include/errno.h"
#include "lib/lib.h"
#include "vm/vm.h"
#include "buffer.h"
#include "trace-events.h"
#include "trace.h"


#define TRACE_SAMPLE_PERIOD_US 1000U /* default sampling period */
#define TRACE_DEPTH            16U   /* default frame-pointer chain length */
#define TRACE_KDEPTH           12U   /* kernel frames recorded */
#define TRACE_USTACK_SPAN      (1UL << 20) /* stack extent assumed when the thread's stack is not known */


/* Frame buffers of one CPU: an event is built with interrupts disabled, so one set per CPU suffices */
typedef struct {
	u64 kframes[TRACE_KDEPTH];
	u64 frames[PERF_TRACE_DEPTH_MAX];
} trace_frames_t;


typedef struct {
	const void *data;
	size_t sz;
} trace_part_t;


static struct {
	/*
	 * Treat `running` as atomic to reduce overhead on the kernel when the tracing is disabled
	 * - there is only one writer at a time (perf_trace{Start,Finish}()) and multiple readers
	 * (trace events doing trace_isRunning()). Due to eventual consistency in the readers
	 * we may lose some events, but we may lose them anyway as the invocation of
	 * trace_start() naturally races with kernel events occurring in the meantime.
	 *
	 * `running` under spinlock is always consistent.
	 */
	volatile int running;
	spinlock_t spinlock;

	/* guarded by spinlock */
	int stopped;
	int startPending; /* trace_start guard flag */

	int epoch;
	u64 prev;
	unsigned int flags;

	u8 errorFlags;
	u64 eventDelayCount;
	u64 eventDelayTsOffset; /* offset relative to startTimestamp */
	u64 startTimestamp;

	u64 eventDiscardCount;

	/* Profiling, set before tracing is enabled */
	perf_trace_cfg_t cfg;
	u64 *sampleNext;         /* per CPU: time of its next sample */
	trace_frames_t *frames; /* per CPU */

	/* Deferred waits (cfg.waitMinUs), guarded by stashLock: threads_common.spinlock -> stashLock -> spinlock */
	spinlock_t stashLock;
	struct _trace_stash_t **stash; /* TRACE_STASH_SLOTS, NULL when not deferring */
	size_t stashSz;                /* payload bytes of a slot */
	u64 stashDropped;              /* waits not recorded: no free slot */
} trace_common;


#define TRACE_NON_MONOTONICITY (1U << 1)
#define TRACE_EVENT_DELAYED    (1U << 2)
#define TRACE_BUFFER_WRITE_ERR (1U << 3)
#define TRACE_EVENT_DISCARDED  (1U << 4)


static u32 _getUsFromStart(void)
{
	u64 now = (u64)hal_timerGetUs();

	if (now < trace_common.prev) {
		trace_common.errorFlags |= TRACE_NON_MONOTONICITY;
		now = trace_common.prev;
	}

	trace_common.prev = now;

	/* Intentional downcast to u32 - traces >1h are not supported */
	return (u32)(now - trace_common.startTimestamp);
}


/* Writes one event whose payload is the concatenation of parts */
static void _writeEventParts(u8 cpuChan, u8 event, const trace_part_t *parts, size_t nparts, u32 *ts)
{
	u32 eventTs;
	ssize_t ret, try = 0;
	size_t eventSz = sizeof(eventTs) + sizeof(event);
	size_t avail, i;
	u8 chan = cpuChan + (u8)hal_cpuGetID() * (u8)trace_channel_count;

	struct {
		u32 ts;
		u8 eventId;
	} __attribute__((packed)) ev;

	for (i = 0; i < nparts; i++) {
		eventSz += parts[i].sz;
	}

	if (ts == NULL || *ts == 0U) {
		eventTs = _getUsFromStart();
		if (ts != NULL) {
			*ts = eventTs;
		}
	}
	else {
		/* use timestamp provided by the caller */
		eventTs = *ts;
	}

	ret = _trace_bufferAvail(chan);
	if (ret < 0) {
		trace_common.errorFlags |= TRACE_BUFFER_WRITE_ERR;
		return;
	}

	avail = (size_t)ret;
	if (avail < eventSz) {
		if ((trace_common.flags & PERF_TRACE_FLAG_ROLLING) != 0U) {
			(void)_trace_bufferDiscard(chan, eventSz - avail);
		}
		else {
			try = _trace_bufferWaitUntilAvail(chan, eventSz);
			if (try < 0) {
				trace_common.errorFlags |= TRACE_EVENT_DISCARDED;
				trace_common.eventDiscardCount++;
				return;
			}
		}
	}

	ev.ts = eventTs;
	ev.eventId = event;
	ret = _trace_bufferWrite(chan, &ev, sizeof(ev));
	for (i = 0; (i < nparts) && (ret >= 0); i++) {
		if (parts[i].sz != 0U) {
			ret = _trace_bufferWrite(chan, parts[i].data, parts[i].sz);
		}
	}

	if (ret < 0) {
		trace_common.errorFlags |= TRACE_BUFFER_WRITE_ERR;
	}
	if (try > 0) {
		/*
		 * Record first occurrence of event delay to caution the user about possible
		 * loss of timestamp precision. This may happen if e.g. the buffer is implemented as RTT
		 * and the receiver (debug probe) can't keep up with the event generation rate
		 */
		trace_common.errorFlags |= TRACE_EVENT_DELAYED;
		trace_common.eventDelayCount++;
		trace_common.eventDelayTsOffset = _getUsFromStart();
	}
}


static void _writeEvent(u8 cpuChan, u8 event, const void *data, size_t sz, u32 *ts)
{
	trace_part_t part = { .data = data, .sz = sz };

	_writeEventParts(cpuChan, event, &part, 1, ts);
}


/* WARN: should be callable from interrupt handler */
void trace_writeEvent(u8 cpuChan, u8 event, const void *data, size_t sz, u32 *ts)
{
	spinlock_ctx_t sc;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0) {
		_writeEvent(cpuChan, event, data, sz, ts);
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);
}


void _trace_updateLockEpoch(lock_t *lock)
{
	int prev = _proc_lockSetTraceEpoch(lock, trace_common.epoch);

	if (prev != trace_common.epoch) {
		_trace_eventLockName(lock);
	}
}


/* WARN: eventually consistent */
int trace_isRunning(void)
{
	return trace_common.running;
}


/*
 * Profiling events: thread_sample and thread_wait.
 *
 * Both describe a thread by its kernel frame-pointer chain (if it is in the kernel) and its user
 * state: the registers it entered the kernel with (or was interrupted at), the frame-pointer chain
 * of its stack, and optionally a copy of the top of its stack, from which the reader can recover
 * callers of code built without frame pointers. User memory is only read in the address space of
 * the current thread and only where hal_cpuCanRead() says a read cannot fault, as these events are
 * built in interrupt context or with threads_common.spinlock set.
 */

#ifdef HAL_PERF_FRAMES

/* User half of a sample or wait record; must mirror tsdl/metadata */
typedef struct {
	u64 pc;
	u64 lr;
	u64 sp;
	u64 fp;
	u8 nframes;
} __attribute__((packed)) trace_ureg_t;


/* Registers t entered the kernel with (syscall, exception), or NULL for a kernel thread */
static cpu_context_t *_userContext(const thread_t *t)
{
	cpu_context_t *uctx;

	if ((t->process == NULL) || (t->kstack == NULL) || (t->kstacksz < sizeof(cpu_context_t))) {
		return NULL;
	}

	/* An entry from EL0 saves the context at the top of the thread's kernel stack */
	uctx = (cpu_context_t *)((char *)t->kstack + t->kstacksz - sizeof(cpu_context_t));

	return (hal_cpuSupervisorMode(uctx) == 0) ? uctx : NULL;
}


static unsigned int _kernelFrames(const thread_t *t, ptr_t fp, u64 *kframes)
{
	ptr_t lo = (ptr_t)t->kstack;

	if (lo == 0U) {
		return 0;
	}

	return hal_cpuBacktrace(fp, lo, lo + t->kstacksz, kframes, TRACE_KDEPTH);
}


/*
 * Fills ureg and frames from uctx. If readMem is set (t's address space is the current one),
 * walks t's user stack and returns how many words from sp may be copied, at most stackBytes.
 */
static size_t _userRecord(const thread_t *t, cpu_context_t *uctx, int readMem, size_t stackBytes, trace_ureg_t *ureg, u64 *frames)
{
	ptr_t sp, end, lo, page;
	size_t words;

	hal_memset(ureg, 0, sizeof(*ureg));
	if (uctx == NULL) {
		return 0;
	}

	sp = (ptr_t)hal_cpuGetUserSP(uctx);
	ureg->pc = hal_cpuGetPC(uctx);
	ureg->lr = hal_cpuGetLR(uctx);
	ureg->fp = hal_cpuGetFP(uctx);
	ureg->sp = sp;

	if ((readMem == 0) || (sp == 0U) || (sp >= (ptr_t)VADDR_USR_MAX) || ((sp & 7U) != 0U)) {
		return 0;
	}

	/* Bound every read by the thread's stack, or a span of user space when it is not known */
	lo = (ptr_t)t->ustack;
	if ((lo != 0U) && (sp >= lo) && (sp < lo + t->ustacksz)) {
		end = lo + t->ustacksz;
	}
	else {
		end = ((ptr_t)VADDR_USR_MAX - sp > TRACE_USTACK_SPAN) ? sp + TRACE_USTACK_SPAN : (ptr_t)VADDR_USR_MAX;
	}

	ureg->nframes = (u8)hal_cpuBacktrace(ureg->fp, sp, end, frames, trace_common.cfg.depth);

	words = min(stackBytes, (size_t)(end - sp)) / sizeof(u64);
	for (page = sp & ~((ptr_t)SIZE_PAGE - 1U); page < sp + words * sizeof(u64); page += SIZE_PAGE) {
		if (hal_cpuCanRead(page) == 0) {
			words = (page > sp) ? (size_t)(page - sp) / sizeof(u64) : 0U;
			break;
		}
	}

	return words;
}


void _trace_sample(const thread_t *t, cpu_context_t *ctx)
{
	struct {
		u16 tid;
		u8 mode; /* 0: user, 1: kernel on behalf of a process, 2: kernel thread */
		u64 kpc;
		u8 nkframes;
	} __attribute__((packed)) head;
	trace_ureg_t ureg;
	trace_part_t parts[6];
	cpu_context_t *uctx;
	trace_frames_t *f;
	unsigned int cpu = hal_cpuGetID(), nk = 0;
	u16 nstack;
	u64 now;
	spinlock_ctx_t sc;

	if ((t == NULL) || ((trace_common.flags & PERF_TRACE_FLAG_SAMPLE) == 0U)) {
		return;
	}

	/* The timer interrupt also comes early, at wakeup deadlines: keep a steady period (1/8 jitter) */
	now = (u64)hal_timerGetUs();
	if (now < trace_common.sampleNext[cpu]) {
		return;
	}
	trace_common.sampleNext[cpu] = now + trace_common.cfg.samplePeriodUs - trace_common.cfg.samplePeriodUs / 8U;

	f = &trace_common.frames[cpu];
	head.tid = (u16)proc_getTid(t);
	if (hal_cpuSupervisorMode(ctx) == 0) {
		head.mode = 0;
		head.kpc = 0;
		uctx = ctx;
	}
	else {
		head.mode = (t->process != NULL) ? 1U : 2U;
		head.kpc = hal_cpuGetPC(ctx);
		nk = _kernelFrames(t, hal_cpuGetFP(ctx), f->kframes);
		uctx = _userContext(t);
	}
	head.nkframes = (u8)nk;

	/* t runs on this CPU, so its address space is the current one */
	nstack = (u16)_userRecord(t, uctx, 1, trace_common.cfg.sampleStack, &ureg, f->frames);

	parts[0] = (trace_part_t) { &head, sizeof(head) };
	parts[1] = (trace_part_t) { f->kframes, nk * sizeof(u64) };
	parts[2] = (trace_part_t) { &ureg, sizeof(ureg) };
	parts[3] = (trace_part_t) { f->frames, (size_t)ureg.nframes * sizeof(u64) };
	parts[4] = (trace_part_t) { &nstack, sizeof(nstack) };
	parts[5] = (trace_part_t) { (const void *)(ptr_t)ureg.sp, (size_t)nstack * sizeof(u64) };

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0) {
		_writeEventParts((u8)trace_channel_event, TRACE_EVENT_THREAD_SAMPLE, parts, 6, NULL);
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);
}


/* thread_wait payload head; must mirror tsdl/metadata */
typedef struct {
	u16 tid;
	u8 flags;
	u32 queue;
	u32 timeout; /* us left until the deadline when the wait began, 0: none */
	u32 blocked; /* us the wait lasted (deferred), 0: written when it began */
	u64 args[4]; /* syscall arguments */
	u8 nkframes;
} __attribute__((packed)) trace_waithead_t;


/*
 * Deferred waits (cfg.waitMinUs != 0). A wait is recorded when it begins, while its user stack is
 * the current address space and readable, but into a slot of the waiting thread instead of the
 * trace. Only when the wait ends (or the trace stops) and has lasted waitMinUs is it written. Most
 * waits are far shorter than anything worth explaining, so they then cost no trace space.
 */
#define TRACE_STASH_SLOTS 1024U
#define TRACE_STASH_PROBE 8U

typedef struct _trace_stash_t {
	int tid;
	int used;
	u64 start;
	size_t len;
	u8 data[]; /* thread_wait payload */
} trace_stash_t;


/* With stashLock set. alloc: a free slot if tid has none */
static trace_stash_t *_stashFind(int tid, int alloc)
{
	trace_stash_t *s, *free = NULL;
	unsigned int i;

	for (i = 0; i < TRACE_STASH_PROBE; i++) {
		s = trace_common.stash[((unsigned int)tid + i) % TRACE_STASH_SLOTS];
		if (s->used == 0) {
			free = (free == NULL) ? s : free;
		}
		else if (s->tid == tid) {
			return s;
		}
	}

	return (alloc != 0) ? free : NULL;
}


static void _stashPut(int tid, const trace_part_t *parts, size_t nparts)
{
	trace_stash_t *s;
	spinlock_ctx_t sc;
	size_t i, len = 0;

	hal_spinlockSet(&trace_common.stashLock, &sc);
	s = (trace_common.stash != NULL) ? _stashFind(tid, 1) : NULL;
	if (s == NULL) {
		trace_common.stashDropped++;
	}
	else {
		/* parts never exceed stashSz: it is sized from the same limits (_stashAlloc()) */
		for (i = 0; i < nparts; i++) {
			hal_memcpy(s->data + len, parts[i].data, parts[i].sz);
			len += parts[i].sz;
		}
		s->tid = tid;
		s->used = 1;
		s->start = (u64)hal_timerGetUs();
		s->len = len;
	}
	hal_spinlockClear(&trace_common.stashLock, &sc);
}


/* With stashLock set: writes the stashed wait, now blocked us long */
static void _stashEmit(trace_stash_t *s, u64 blocked, u8 flags)
{
	trace_waithead_t *head = (trace_waithead_t *)s->data;
	trace_part_t part = { .data = s->data, .sz = s->len };
	spinlock_ctx_t sc;

	head->flags |= flags;
	head->blocked = (blocked > 0xffffffffU) ? 0xffffffffU : (u32)blocked;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0) {
		_writeEventParts((u8)trace_channel_event, TRACE_EVENT_THREAD_WAIT, &part, 1, NULL);
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);
}


void _trace_threadWoken(const thread_t *t)
{
	trace_stash_t *s;
	spinlock_ctx_t sc;
	u64 blocked;

	hal_spinlockSet(&trace_common.stashLock, &sc);
	s = (trace_common.stash != NULL) ? _stashFind(proc_getTid(t), 0) : NULL;
	if (s != NULL) {
		blocked = (u64)hal_timerGetUs() - s->start;
		if (blocked >= trace_common.cfg.waitMinUs) {
			_stashEmit(s, blocked, TRACE_WAIT_DEFERRED);
		}
		s->used = 0;
	}
	hal_spinlockClear(&trace_common.stashLock, &sc);
}


/* A wakeup that ends a stashed wait shorter than waitMinUs is left out with it */
static int _stashIsShort(int tid)
{
	trace_stash_t *s;
	spinlock_ctx_t sc;
	int isShort = 0;

	hal_spinlockSet(&trace_common.stashLock, &sc);
	s = (trace_common.stash != NULL) ? _stashFind(tid, 0) : NULL;
	if ((s != NULL) && (((u64)hal_timerGetUs() - s->start) < trace_common.cfg.waitMinUs)) {
		isShort = 1;
	}
	hal_spinlockClear(&trace_common.stashLock, &sc);

	return isShort;
}


/* The trace stops: the waits still going on and long enough are written as open */
static void _stashFlush(void)
{
	trace_stash_t *s;
	spinlock_ctx_t sc;
	u64 now = (u64)hal_timerGetUs();
	unsigned int i;

	hal_spinlockSet(&trace_common.stashLock, &sc);
	for (i = 0; (trace_common.stash != NULL) && (i < TRACE_STASH_SLOTS); i++) {
		s = trace_common.stash[i];
		if ((s->used != 0) && ((now - s->start) >= trace_common.cfg.waitMinUs)) {
			_stashEmit(s, now - s->start, TRACE_WAIT_DEFERRED | TRACE_WAIT_OPEN);
		}
		s->used = 0;
	}
	hal_spinlockClear(&trace_common.stashLock, &sc);
}


static void _stashFree(void)
{
	trace_stash_t **stash;
	spinlock_ctx_t sc;
	unsigned int i;

	hal_spinlockSet(&trace_common.stashLock, &sc);
	stash = trace_common.stash;
	trace_common.stash = NULL;
	hal_spinlockClear(&trace_common.stashLock, &sc);

	if (stash != NULL) {
		for (i = 0; i < TRACE_STASH_SLOTS; i++) {
			vm_kfree(stash[i]);
		}
		vm_kfree(stash);
	}
}


static int _stashAlloc(void)
{
	trace_stash_t **stash;
	unsigned int i;
	size_t sz;

	if (trace_common.cfg.waitMinUs == 0U) {
		return EOK;
	}

	/* the largest thread_wait payload under the current configuration */
	sz = sizeof(trace_waithead_t) + TRACE_KDEPTH * sizeof(u64) + sizeof(trace_ureg_t) +
			trace_common.cfg.depth * sizeof(u64) + sizeof(u16) + trace_common.cfg.waitStack;

	stash = vm_kmalloc(TRACE_STASH_SLOTS * sizeof(*stash));
	if (stash == NULL) {
		return -ENOMEM;
	}
	for (i = 0; i < TRACE_STASH_SLOTS; i++) {
		stash[i] = vm_kmalloc(sizeof(trace_stash_t) + sz);
		if (stash[i] == NULL) {
			while (i > 0U) {
				vm_kfree(stash[--i]);
			}
			vm_kfree(stash);
			return -ENOMEM;
		}
		stash[i]->used = 0;
	}

	trace_common.stashSz = sz;
	trace_common.stashDropped = 0;
	trace_common.stash = stash; /* tracing is not enabled yet */

	return EOK;
}


/*
 * Writes thread_wait for t, sleeping. kfp is the kernel frame pointer to walk from. With
 * existing set, t is not the current thread (its user memory is not read) and the trace is being
 * started, so the event is written without the trace spinlock, as _emitThreadinfo() does.
 */
static void _threadWaitRecord(const thread_t *t, ptr_t kfp, int existing)
{
	trace_waithead_t head;
	trace_ureg_t ureg;
	trace_part_t parts[6];
	cpu_context_t *uctx;
	trace_frames_t *f = &trace_common.frames[hal_cpuGetID()];
	unsigned int nk, i;
	u16 nstack;
	u64 now, left;
	spinlock_ctx_t sc;

	head.tid = (u16)proc_getTid(t);
	head.flags = (existing != 0) ? TRACE_WAIT_EXISTING : 0U;
	head.queue = (u32)(ptr_t)t->wait;
	head.timeout = 0;
	head.blocked = 0;
	if (t->wakeup != 0) {
		now = (u64)hal_timerGetUs();
		left = ((u64)t->wakeup > now) ? (u64)t->wakeup - now : 1U;
		head.timeout = (left > 0xffffffffU) ? 0xffffffffU : (u32)left;
	}

	uctx = _userContext(t);
	for (i = 0; i < 4U; i++) {
		head.args[i] = (uctx != NULL) ? hal_cpuGetArg(uctx, i) : 0U;
	}

	nk = _kernelFrames(t, kfp, f->kframes);
	head.nkframes = (u8)nk;
	nstack = (u16)_userRecord(t, uctx, (existing == 0) ? 1 : 0, trace_common.cfg.waitStack, &ureg, f->frames);

	parts[0] = (trace_part_t) { &head, sizeof(head) };
	parts[1] = (trace_part_t) { f->kframes, nk * sizeof(u64) };
	parts[2] = (trace_part_t) { &ureg, sizeof(ureg) };
	parts[3] = (trace_part_t) { f->frames, (size_t)ureg.nframes * sizeof(u64) };
	parts[4] = (trace_part_t) { &nstack, sizeof(nstack) };
	parts[5] = (trace_part_t) { (const void *)(ptr_t)ureg.sp, (size_t)nstack * sizeof(u64) };

	if (existing != 0) {
		_writeEventParts((u8)trace_channel_event, TRACE_EVENT_THREAD_WAIT, parts, 6, NULL);
		return;
	}

	if (trace_common.cfg.waitMinUs != 0U) {
		_stashPut(proc_getTid(t), parts, 6);
		return;
	}

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0) {
		_writeEventParts((u8)trace_channel_event, TRACE_EVENT_THREAD_WAIT, parts, 6, NULL);
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);
}


void _trace_threadWait(const thread_t *t)
{
	_threadWaitRecord(t, (ptr_t)__builtin_frame_address(0), 0);
}


static void _emitWaitingCb(const thread_t *t, void *arg)
{
	(void)arg;

	/* t switched out in the kernel; its saved context holds the frame pointer of that point */
	_threadWaitRecord(t, (t->context != NULL) ? hal_cpuGetFP(t->context) : 0U, 1);
}


static void _emitWaiting(void)
{
	proc_threadsIterWaiting(_emitWaitingCb, NULL);
}


static int _sampleSupported(void)
{
	return 1;
}

#else

void _trace_sample(const thread_t *t, cpu_context_t *ctx)
{
	(void)t;
	(void)ctx;
}


void _trace_threadWait(const thread_t *t)
{
	(void)t;
}


void _trace_threadWoken(const thread_t *t)
{
	(void)t;
}


static int _stashIsShort(int tid)
{
	(void)tid;
	return 0;
}


static void _stashFlush(void)
{
}


static void _stashFree(void)
{
}


static int _stashAlloc(void)
{
	return EOK;
}


static void _emitWaiting(void)
{
}


static int _sampleSupported(void)
{
	return 0;
}

#endif /* HAL_PERF_FRAMES */


void _trace_threadWakeup(const thread_t *t, const thread_t *waker, unsigned int cause)
{
	struct {
		u16 tid;
		u16 waker;
		u8 cause;
	} __attribute__((packed)) ev;
	spinlock_ctx_t sc;

	if ((trace_common.cfg.waitMinUs != 0U) && (_stashIsShort(proc_getTid(t)) != 0)) {
		return;
	}

	ev.tid = (u16)proc_getTid(t);
	ev.waker = (waker != NULL) ? (u16)proc_getTid(waker) : 0U;
	ev.cause = (u8)cause;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0) {
		_writeEvent((u8)trace_channel_event, TRACE_EVENT_THREAD_WAKEUP, &ev, sizeof(ev), NULL);
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);
}


static void _emitThreadsCb(void *arg, threadinfo_t *tinfo)
{
	struct {
		u16 pid;
		u16 tid;
		s8 priority;
		char name[128];
	} __attribute__((packed)) ev;

	ev.tid = (u16)tinfo->tid;
	ev.priority = (s8)tinfo->priority;
	ev.pid = (u16)tinfo->pid;

	hal_memcpy(ev.name, tinfo->name, sizeof(tinfo->name));

	_writeEvent((u8)trace_channel_meta, TRACE_EVENT_THREAD_CREATE, &ev, sizeof(ev), NULL);
}


static void _emitThreadinfo(void)
{
	proc_threadsIter(PH_THREADINFO_TID | PH_THREADINFO_PRIO | PH_THREADINFO_NAME, _emitThreadsCb, NULL);
}


static void _enableTracing(int enable)
{
	trace_common.running = enable;
	_hal_interruptsTrace(enable);
}


static int getChannelCount(void)
{
	return (int)hal_cpuGetCount() * (int)trace_channel_count;
}


static int trace_setConfig(unsigned int flags, const void *arg, size_t sz)
{
	perf_trace_cfg_t cfg = { 0 };

	if ((trace_common.sampleNext == NULL) || (trace_common.frames == NULL)) {
		return -ENOMEM;
	}

	if (arg != NULL) {
		/* an older caller passes fewer fields: the rest stay 0 */
		if (sz > sizeof(cfg)) {
			return -EINVAL;
		}
		hal_memcpy(&cfg, arg, sz);
	}

	if (((flags & PERF_TRACE_FLAG_SAMPLE) != 0U) && (_sampleSupported() == 0)) {
		return -ENOSYS;
	}

	if ((cfg.depth > PERF_TRACE_DEPTH_MAX) || (cfg.sampleStack > PERF_TRACE_USTACK_MAX) || (cfg.waitStack > PERF_TRACE_USTACK_MAX)) {
		return -EINVAL;
	}

	/* deferred waits are kept per thread until they end: their stacks are kept small */
	if ((cfg.waitMinUs != 0U) && (cfg.waitStack > PERF_TRACE_WAITSTACK_DEFERRED_MAX)) {
		return -EINVAL;
	}

	if (cfg.samplePeriodUs == 0U) {
		cfg.samplePeriodUs = TRACE_SAMPLE_PERIOD_US;
	}
	if (cfg.depth == 0U) {
		cfg.depth = TRACE_DEPTH;
	}

	/* No CPU samples now: tracing is not enabled yet */
	trace_common.cfg = cfg;
	hal_memset(trace_common.sampleNext, 0, sizeof(*trace_common.sampleNext) * hal_cpuGetCount());

	return EOK;
}


int trace_start(unsigned int flags, const void *arg, size_t sz)
{
	spinlock_ctx_t sc;
	int ret;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0 || trace_common.stopped != 0 || trace_common.startPending != 0) {
		hal_spinlockClear(&trace_common.spinlock, &sc);
		return -EINPROGRESS;
	}
	trace_common.startPending = 1;
	hal_spinlockClear(&trace_common.spinlock, &sc);

	ret = trace_setConfig(flags, arg, sz);
	if (ret == EOK) {
		ret = _stashAlloc();
	}
	if (ret == EOK) {
		ret = _trace_bufferStart();
		if (ret < 0) {
			_stashFree();
		}
	}
	if (ret < 0) {
		hal_spinlockSet(&trace_common.spinlock, &sc);
		trace_common.startPending = 0;
		hal_spinlockClear(&trace_common.spinlock, &sc);
		return ret;
	}

	if (_trace_bufferDiscard(0, 0) == -ENOSYS) {
		/* If discarding is unsupported by the buffer backend, ignore the flag */
		flags &= ~PERF_TRACE_FLAG_ROLLING;
	}

	/* Must be set before _emitThreadinfo() as _writeEvent() depends on these */
	trace_common.flags = flags;
	trace_common.startTimestamp = (u64)hal_timerGetUs();
	trace_common.prev = trace_common.startTimestamp;
	trace_common.errorFlags = 0;
	trace_common.eventDelayCount = 0;
	trace_common.eventDiscardCount = 0;

	/* Without spinlock - trace is not enabled yet, so there's no concurrent access */
	_emitThreadinfo();
	/* What the threads asleep now wait for: their waits began before the trace */
	_emitWaiting();

	trace_common.epoch++;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	trace_common.startPending = 0;
	_enableTracing(1);
	hal_spinlockClear(&trace_common.spinlock, &sc);

	return (int)getChannelCount();
}


int trace_read(u8 chan, void *buf, size_t bufsz)
{
	spinlock_ctx_t sc;
	int ret, running;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	running = trace_common.running;
	if (chan < (u8)getChannelCount() && (running != 0 || trace_common.stopped != 0)) {
		ret = _trace_bufferRead(chan, buf, bufsz);
	}
	else {
		ret = -EINVAL;
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);

	return ret;
}


int trace_stop(void)
{
	int ret = EOK, running;
	spinlock_ctx_t sc;

	/* while still running: the waits going on now are written as open */
	_stashFlush();

	hal_spinlockSet(&trace_common.spinlock, &sc);
	running = trace_common.running;
	if (trace_common.stopped == 0 && running != 0) {
		_enableTracing(0);
		trace_common.stopped = 1;
		ret = getChannelCount();
	}
	else {
		ret = -EINVAL;
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);

	return ret;
}


int trace_finish(void)
{
	spinlock_ctx_t sc;
	int ret = EOK;
	u8 errorFlags = 0;
	u64 eventDelayCount = 0;
	u64 eventDiscardCount = 0;
	u64 eventDelayTimestamp = 0;
	u64 startTimestamp = 0;
	u64 stopTimestamp = 0;

	hal_spinlockSet(&trace_common.spinlock, &sc);
	if (trace_common.running != 0 || trace_common.stopped != 0) {
		_enableTracing(0);
		trace_common.stopped = 0;
		eventDelayCount = trace_common.eventDelayCount;
		trace_common.eventDelayCount = 0;
		eventDiscardCount = trace_common.eventDiscardCount;

		startTimestamp = trace_common.startTimestamp;
		stopTimestamp = startTimestamp + _getUsFromStart();
		eventDelayTimestamp = startTimestamp + trace_common.eventDelayTsOffset;

		/* read last: _getUsFromStart() above could set an error flag */
		errorFlags = trace_common.errorFlags;
	}
	else {
		ret = -EINVAL;
	}
	hal_spinlockClear(&trace_common.spinlock, &sc);

	if (ret == EOK) {
		if ((errorFlags & TRACE_NON_MONOTONICITY) != 0U) {
			lib_printf("kernel (%s:%d): timer non-monotonicity detected during event gathering\n", __func__, __LINE__);
		}

		if ((errorFlags & TRACE_EVENT_DELAYED) != 0U) {
			lib_printf("kernel (%s:%d): event delay detected %llu times - event receiver couldn't keep up\n", __func__, __LINE__, eventDelayCount);
			lib_printf("kernel (%s:%d): start ts=%lld delay ts=%lld stop ts=%lld\n", __func__, __LINE__, startTimestamp, eventDelayTimestamp, stopTimestamp);
		}

		if ((errorFlags & TRACE_BUFFER_WRITE_ERR) != 0U) {
			lib_printf("kernel (%s:%d): buffer write error detected\n", __func__, __LINE__);
		}

		if ((errorFlags & TRACE_EVENT_DISCARDED) != 0U) {
			lib_printf("kernel (%s:%d): event discard detected (%llu events: a channel was full - read faster or record less)\n",
					__func__, __LINE__, eventDiscardCount);
		}

		if (trace_common.stashDropped != 0U) {
			lib_printf("kernel (%s:%d): %llu waits not recorded (more threads waiting than wait slots)\n", __func__, __LINE__,
					trace_common.stashDropped);
		}

		_stashFree();
		ret = _trace_bufferFinish();
	}

	return ret;
}


int _trace_init(vm_map_t *kmap)
{
	trace_common.running = 0;
	trace_common.stopped = 0;
	trace_common.startPending = 0;

	trace_common.epoch = 0;

	hal_spinlockCreate(&trace_common.spinlock, "trace.spinlock");
	hal_spinlockCreate(&trace_common.stashLock, "trace.stashLock");
	trace_common.stash = NULL;

	trace_common.sampleNext = vm_kmalloc(sizeof(*trace_common.sampleNext) * hal_cpuGetCount());
	trace_common.frames = vm_kmalloc(sizeof(*trace_common.frames) * hal_cpuGetCount());
	if ((trace_common.sampleNext == NULL) || (trace_common.frames == NULL)) {
		return -ENOMEM;
	}

	return trace_bufferInit(kmap);
}
