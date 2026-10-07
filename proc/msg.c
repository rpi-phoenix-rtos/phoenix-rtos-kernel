/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Messages
 *
 * Copyright 2017, 2018 Phoenix Systems
 * Author: Jakub Sejdak, Pawel Pisarczyk, Aleksander Kaminski, Jan Sikorski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "include/errno.h"
#include "hal/timer.h"
#include "lib/lib.h"
#include "proc.h"
#include "perf/trace-events.h"


#define FLOOR(x) ((x) & ~(SIZE_PAGE - 1U))
#define CEIL(x)  (((x) + SIZE_PAGE - 1U) & ~(SIZE_PAGE - 1U))


/* clang-format off */
enum { msg_rejected = -1, msg_waiting = 0, msg_received, msg_responded };
/* clang-format on */


/* A sender told to exit waits in steps of this long, and reports a request still pending after MSG_EXIT_REPORT_US */
#define MSG_EXIT_STEP_US   (1000LL * 1000LL)
#define MSG_EXIT_REPORT_US (2LL * 1000LL * 1000LL)


/* What a sender that has been told to exit knows about its wait (proc_sendEx) */
typedef struct {
	time_t since;    /* When it noticed, 0 before */
	time_t deadline; /* End of its next wait step */
	int reported;
} msg_exitwait_t;


static struct {
	vm_map_t *kmap;
	vm_object_t *kernel;
	unsigned int deviceRefusals;
	unsigned int exitReports;
} msg_common;


/* Flags of the source mapping holding vaddr (in the kernel map if vaddr is not in srcmap) */
static int msg_srcFlags(vm_map_t *srcmap, void *vaddr)
{
	if (pmap_belongs(&srcmap->pmap, vaddr) == 0) {
		srcmap = msg_common.kmap;
	}

	return vm_mapFlags(srcmap, vaddr);
}


/*
 * Device memory is not a message payload where the HAL says unaligned accesses to it fault
 * (PGHD_DEV_ALIGNED_ONLY).
 *
 * The kernel copies payloads with hal_memcpy, which makes unaligned accesses: the partial first and
 * last pages through a kernel view with the memory type of the sender's mapping (msg_map, and back
 * in proc_respond), small payloads straight from and to the sender's buffer (msg_ipack, and back in
 * proc_sendEx). An alignment fault cannot be resolved by mapping anything, so the copying thread
 * would take the same fault on every return from the handler. The pages the kernel does not copy
 * would reach the receiver as device memory, where its own unaligned accesses fault alike. So such a
 * payload is refused, and the sender's call fails with -EINVAL as for a payload that cannot be mapped.
 */
static int msg_isDevice(int flags)
{
#ifdef PGHD_DEV_ALIGNED_ONLY
	return ((flags >= 0) && (((vm_flags_t)flags & MAP_DEVICE) != 0U)) ? 1 : 0;
#else
	(void)flags;
	return 0;
#endif
}


/*
 * Does the first or the last page of the user payload [data, data + size) lie in device memory?
 * For msg_ipack and msg_opack, which run before a payload is packed. A buffer of the kernel's own
 * is taken as memory: that avoids a kernel map lookup on every small message the kernel sends.
 */
static int msg_payloadIsDevice(const process_t *proc, const void *data, size_t size)
{
#ifdef PGHD_DEV_ALIGNED_ONLY
	void *first = (void *)(ptr_t)data, *last = first + size - 1U;

	if ((proc == NULL) || (pmap_belongs(&proc->mapp->pmap, first) == 0)) {
		return 0;
	}

	if (msg_isDevice(vm_mapFlags(proc->mapp, first)) != 0) {
		return 1;
	}

	return ((FLOOR((ptr_t)last) != FLOOR((ptr_t)first)) && (msg_isDevice(vm_mapFlags(proc->mapp, last)) != 0)) ? 1 : 0;
#else
	(void)proc;
	(void)data;
	(void)size;
	return 0;
#endif
}


static void msg_reportDevice(const process_t *from, const void *data, size_t size)
{
	/* A diagnostic: unsynchronised, and bounded so that a sender retrying in a loop cannot flood the console */
	if (msg_common.deviceRefusals < 8U) {
		msg_common.deviceRefusals++;
		lib_printf("msg: refused a payload in device memory (%zu bytes at %p) from %s (PID %u)\n", size, data,
				((from != NULL) && (from->path != NULL)) ? from->path : "kernel", (from != NULL) ? (unsigned int)process_getPid(from) : 0U);
	}
}


static void *msg_map(int dir, kmsg_t *kmsg, void *data, size_t size, process_t *from, process_t *to)
{
	void *w = NULL, *vaddr;
	size_t boffs, eoffs;
	u8 bone, eone;
	size_t n = 0, i;
	vm_attr_t attr;
	vm_prot_t prot;
	page_t *nep = NULL, *nbp = NULL;
	vm_map_t *srcmap, *dstmap;
	struct _kmsg_layout_t *ml = (dir != 0) ? &kmsg->o : &kmsg->i;
	int err;
	vm_flags_t flags, eflags;
	addr_t bpa, pa, epa;

	if ((size == 0U) || (data == NULL)) {
		return NULL;
	}

	attr = PGHD_READ | PGHD_PRESENT;
	prot = PROT_READ;

	if (dir != 0) {
		attr |= PGHD_WRITE;
		prot |= PROT_WRITE;
	}

	if (to != NULL) {
		attr |= PGHD_USER;
		prot |= PROT_USER;
	}

	boffs = (size_t)(ptr_t)data & (size_t)(SIZE_PAGE - 1U);

	if (FLOOR((ptr_t)data + size) > CEIL((ptr_t)data)) {
		n = (FLOOR((ptr_t)data + size) - CEIL((ptr_t)data)) / SIZE_PAGE;
	}

	if ((boffs != 0U) && (FLOOR((ptr_t)data) == FLOOR((ptr_t)data + size))) {
		/* Data is on one page only and will be copied by boffs handler */
		eoffs = 0U;
	}
	else {
		eoffs = ((size_t)(ptr_t)data + size) & (size_t)(SIZE_PAGE - 1U);
	}

	bone = (boffs != 0U) ? 1U : 0U;
	eone = (eoffs != 0U) ? 1U : 0U;

	srcmap = (from == NULL) ? msg_common.kmap : from->mapp;
	dstmap = (to == NULL) ? msg_common.kmap : to->mapp;

	if ((srcmap == dstmap) && (pmap_belongs(&dstmap->pmap, data) != 0)) {
		return data;
	}

	/* The payload is mapped by the physical addresses of the sender's pages, and a demand-zeroed
	 * page the sender never touched has none yet: pmap_resolve() gives 0 for it. Its frame is
	 * allocated here, while no lock is held. */
	if ((srcmap != msg_common.kmap) && (vm_mapPopulate(srcmap, data, size) < 0)) {
		return NULL;
	}

	w = vm_mapFind(dstmap, NULL, (n + bone + eone) * SIZE_PAGE, MAP_NOINHERIT, prot);
	ml->w = w;
	if (w == NULL) {
		return NULL;
	}

	/* The kernel views of the partial pages take the memory type of the mappings they are in (only) */
	err = msg_srcFlags(srcmap, data);
	if (err < 0) {
		return NULL;
	}
	flags = (vm_flags_t)err & (MAP_UNCACHED | MAP_DEVICE);

	eflags = flags;
	if (eoffs != 0U) {
		err = msg_srcFlags(srcmap, (void *)FLOOR((ptr_t)data + size));
		if (err < 0) {
			return NULL;
		}
		eflags = (vm_flags_t)err & (MAP_UNCACHED | MAP_DEVICE);
	}

	if ((msg_isDevice((int)flags) != 0) || (msg_isDevice((int)eflags) != 0)) {
		msg_reportDevice(from, data, size);
		return NULL;
	}

	attr |= vm_flagsToAttr(flags);

	if (boffs != 0U) {
		ml->boffs = boffs;
		bpa = pmap_resolve(&srcmap->pmap, data) & ~(SIZE_PAGE - 1U);

		nbp = vm_pageAlloc(SIZE_PAGE, PAGE_OWNER_APP);
		ml->bp = nbp;
		if (nbp == NULL) {
			return NULL;
		}

		vaddr = vm_mmap(msg_common.kmap, NULL, NULL, SIZE_PAGE, PROT_READ | PROT_WRITE, VM_OBJ_PHYSMEM, (off_t)bpa, flags);
		ml->bvaddr = vaddr;
		if (vaddr == NULL) {
			return NULL;
		}

		/* Map new page into destination address space */
		if (page_map(&dstmap->pmap, w, nbp->addr, (attr | PGHD_WRITE) & ~PGHD_USER) < 0) {
			return NULL;
		}

		hal_memcpy(w + boffs, vaddr + boffs, (size_t)min(size, SIZE_PAGE - boffs));

		if (page_map(&dstmap->pmap, w, nbp->addr, attr) < 0) {
			return NULL;
		}
	}

	/* Map pages */
	vaddr = (void *)CEIL((ptr_t)data);

	for (i = 0; i < n; i++) {
		pa = pmap_resolve(&srcmap->pmap, vaddr) & ~(SIZE_PAGE - 1U);
		if (page_map(&dstmap->pmap, w + (i + bone) * SIZE_PAGE, pa, attr) < 0) {
			return NULL;
		}
		vaddr += SIZE_PAGE;
	}

	if (eoffs != 0U) {
		ml->eoffs = eoffs;
		vaddr = (void *)FLOOR((ptr_t)data + size);
		epa = pmap_resolve(&srcmap->pmap, vaddr) & ~(SIZE_PAGE - 1U);

		if ((boffs == 0U) || (eoffs >= boffs)) {
			nep = vm_pageAlloc(SIZE_PAGE, PAGE_OWNER_APP);
			ml->ep = nep;
			if (nep == NULL) {
				return NULL;
			}
		}
		else {
			nep = nbp;
		}

		vaddr = vm_mmap(msg_common.kmap, NULL, NULL, SIZE_PAGE, PROT_READ | PROT_WRITE, VM_OBJ_PHYSMEM, (off_t)epa, eflags);
		ml->evaddr = vaddr;
		if (vaddr == NULL) {
			return NULL;
		}

		/* Map new page into destination address space */
		if (page_map(&dstmap->pmap, w + (n + bone) * SIZE_PAGE, nep->addr, (attr | PGHD_WRITE) & ~PGHD_USER) < 0) {
			return NULL;
		}

		hal_memcpy(w + (n + bone) * SIZE_PAGE, vaddr, eoffs);

		if (page_map(&dstmap->pmap, w + (n + bone) * SIZE_PAGE, nep->addr, attr) < 0) {
			return NULL;
		}
	}

	return (w + boffs);
}


/* Releases the shadow pages of an unaligned payload and their kernel mappings */
static void msg_releaseShadow(struct _kmsg_layout_t *ml)
{
	if (ml->bp != NULL) {
		vm_pageFree(ml->bp);
		(void)vm_munmap(msg_common.kmap, ml->bvaddr, SIZE_PAGE);
		ml->bp = NULL;
	}

	if (ml->eoffs != 0U) {
		if (ml->ep != NULL) {
			vm_pageFree(ml->ep);
		}
		(void)vm_munmap(msg_common.kmap, ml->evaddr, SIZE_PAGE);
		ml->eoffs = 0;
		ml->ep = NULL;
	}
}


static void msg_release(kmsg_t *kmsg)
{
	process_t *process;
	vm_map_t *map;

	msg_releaseShadow(&kmsg->i);

	process = proc_current()->process;
	if (process != NULL) {
		map = process->mapp;
	}
	else {
		map = msg_common.kmap;
	}

	if (kmsg->i.w != NULL) {
		(void)vm_munmap(map, kmsg->i.w, CEIL((ptr_t)kmsg->msg.i.data + kmsg->msg.i.size) - FLOOR((ptr_t)kmsg->msg.i.data));
		kmsg->i.w = NULL;
	}

	msg_releaseShadow(&kmsg->o);

	if (kmsg->o.w != NULL) {
		(void)vm_munmap(map, kmsg->o.w, CEIL((ptr_t)kmsg->msg.o.data + kmsg->msg.o.size) - FLOOR((ptr_t)kmsg->msg.o.data));
		kmsg->o.w = NULL;
	}
}


static void msg_ipack(kmsg_t *kmsg)
{
	size_t offset;

	if (kmsg->msg.i.data != NULL) {
		switch (kmsg->msg.type) {
			case mtOpen:
			case mtClose:
				offset = sizeof(kmsg->msg.i.openclose);
				break;

			case mtRead:
			case mtWrite:
			case mtTruncate:
				offset = sizeof(kmsg->msg.i.io);
				break;

			case mtCreate:
				offset = sizeof(kmsg->msg.i.create);
				break;

			case mtLookup:
			case mtDestroy:
			case mtGetAttrAll:
				offset = 0;
				break;

			case mtSetAttr:
			case mtGetAttr:
				offset = sizeof(kmsg->msg.i.attr);
				break;

			case mtLink:
			case mtUnlink:
				offset = sizeof(kmsg->msg.i.ln);
				break;

			case mtReaddir:
				offset = sizeof(kmsg->msg.i.readdir);
				break;

			case mtDevCtl:
			default:
				return;
		}

		if (kmsg->msg.i.size > (sizeof(kmsg->msg.i.raw) - offset)) {
			return;
		}

		/* Left unpacked for msg_map to refuse */
		if (msg_payloadIsDevice(kmsg->src, kmsg->msg.i.data, kmsg->msg.i.size) != 0) {
			return;
		}

		hal_memcpy(kmsg->msg.i.raw + offset, kmsg->msg.i.data, kmsg->msg.i.size);
		kmsg->msg.i.data = kmsg->msg.i.raw + offset;
	}
}


static int msg_opack(kmsg_t *kmsg)
{
	size_t offset;

	if (kmsg->msg.o.data == NULL) {
		return 0;
	}

	switch (kmsg->msg.type) {
		case mtOpen:
		case mtClose:
		case mtRead:
		case mtWrite:
		case mtTruncate:
		case mtDestroy:
		case mtLink:
		case mtUnlink:
		case mtReaddir:
		case mtGetAttrAll:
			offset = 0;
			break;

		case mtCreate:
			offset = sizeof(kmsg->msg.o.create);
			break;

		case mtSetAttr:
		case mtGetAttr:
			offset = sizeof(kmsg->msg.o.attr);
			break;

		case mtLookup:
			offset = sizeof(kmsg->msg.o.lookup);
			break;

		case mtDevCtl:
		default:
			return 0;
	}

	if (kmsg->msg.o.size > (sizeof(kmsg->msg.o.raw) - offset)) {
		return 0;
	}

	/* proc_sendEx would copy the packed response into it: left unpacked for msg_map to refuse */
	if (msg_payloadIsDevice(kmsg->src, kmsg->msg.o.data, kmsg->msg.o.size) != 0) {
		return 0;
	}

	kmsg->msg.o.data = kmsg->msg.o.raw + offset;

	return 1;
}


/*
 * Names, once, a request that keeps an exiting process alive: a process is destroyed (and can be waited
 * for) only once all its threads are gone, and this sender cannot go before its request is answered.
 * Called by the sender itself with no locks held: printing is not safe from the scheduler or under a
 * spinlock. receiver and windows are -1 if the request is not in the rid tree.
 */
static void msg_reportExitWait(const thread_t *sender, u32 port, const kmsg_t *kmsg, int state, int receiver, int windows, int interruptible)
{
	const process_t *proc = sender->process;
	thread_t *t = NULL;
	const process_t *server = NULL;
	const char *what;

	/* Unsynchronised, as msg_reportDevice: a bound, not an exact count */
	if (msg_common.exitReports >= 16U) {
		return;
	}
	msg_common.exitReports++;

	if (state == msg_waiting) {
		lib_printf("proc: pid %d (%s) exit waits for tid %d in msgSend to port %u: not received yet, type %d\n",
				process_getPid(proc), (proc->path != NULL) ? proc->path : "?", proc_getTid(sender), port, kmsg->msg.type);
		return;
	}

	if (receiver >= 0) {
		/* The reference keeps the thread, and so its process, alive while it is printed */
		t = threads_findThread(receiver);
		if (t != NULL) {
			server = t->process;
		}
	}

	if (windows < 0) {
		what = "being answered";
	}
	else if (windows != 0) {
		what = "payload mapped into the server";
	}
	else if (interruptible == 0) {
		what = "uninterruptible kernel request";
	}
	else {
		what = "no payload mapped";
	}

	lib_printf("proc: pid %d (%s) exit waits for tid %d in msgSend to port %u (server pid %d tid %d %s): "
			   "received, type %d, %zu/%zu bytes in/out, %s\n",
			process_getPid(proc), (proc->path != NULL) ? proc->path : "?", proc_getTid(sender), port,
			(server != NULL) ? process_getPid(server) : -1, receiver,
			((server != NULL) && (server->path != NULL)) ? server->path : "?",
			kmsg->msg.type, kmsg->msg.i.size, kmsg->msg.o.size, what);

	if (t != NULL) {
		threads_put(t);
	}
}


/*
 * Takes a received request back from the server for a sender that has been told to exit, if that is
 * safe: returns 1 if so, and the sender may then leave (its kmsg is on its stack).
 *
 * The request must be in the rid tree, i.e. received and not being answered. proc_recv publishes the rid
 * only after it has finished with kmsg, and proc_respond and proc_msgRejectPending take the request out
 * of the tree before they touch it, all under p->lock: so once it is removed here, nothing else can reach
 * it, and the server's msgRespond fails with -ENOENT.
 *
 * And no payload may be mapped into the server (msg_map: a window of the sender's pages, possibly with
 * shadow pages at its ends). The server may be using those pages, and they would be freed with the
 * sender's address space. Packed payloads were copied, so a request without windows leaves the server
 * nothing that refers to the sender. A request with windows still waits for its response.
 */
static int msg_detach(port_t *p, kmsg_t *kmsg)
{
	int detached = 0;

	(void)proc_lockSet(&p->lock);
	if ((kmsg->idlinkage.id >= 0) && (lib_idtreeFind(&p->rid, kmsg->idlinkage.id) == &kmsg->idlinkage) &&
			(kmsg->i.w == NULL) && (kmsg->o.w == NULL)) {
		lib_idtreeRemove(&p->rid, &kmsg->idlinkage);
		detached = 1;
	}
	(void)proc_lockClear(&p->lock);

	return detached;
}


/*
 * One step of the wait of a sender that has been told to exit, called by proc_sendEx with no locks
 * held. state is the request's state as last seen under p->spinlock. Returns 1 if the request was
 * abandoned (msg_detach()). Otherwise sets ew->deadline, the end of the next step: a request that
 * cannot be abandoned yet (still being received, or with payload windows) is retried then.
 */
static int msg_senderExiting(port_t *p, kmsg_t *kmsg, u32 port, int state, int interruptible, msg_exitwait_t *ew)
{
	thread_t *sender = proc_current();
	time_t now;
	int receiver = -1, windows = -1;

	/* An uninterruptible sender is a kernel path that expects the response, whatever happens */
	if ((state == msg_received) && (interruptible != 0) && (msg_detach(p, kmsg) != 0)) {
		return 1;
	}

	proc_gettime(&now, NULL);
	if (ew->since == 0) {
		ew->since = now;
	}

	if ((ew->reported == 0) && ((now - ew->since) >= MSG_EXIT_REPORT_US)) {
		ew->reported = 1;

		if (state == msg_received) {
			/* In the rid tree, the request is not being answered, and what proc_recv wrote is complete */
			(void)proc_lockSet(&p->lock);
			if ((kmsg->idlinkage.id >= 0) && (lib_idtreeFind(&p->rid, kmsg->idlinkage.id) == &kmsg->idlinkage)) {
				receiver = kmsg->receiver;
				windows = ((kmsg->i.w != NULL) || (kmsg->o.w != NULL)) ? 1 : 0;
			}
			(void)proc_lockClear(&p->lock);
		}

		msg_reportExitWait(sender, port, kmsg, state, receiver, windows, interruptible);
	}

	ew->deadline = now + MSG_EXIT_STEP_US;

	return 0;
}


#ifdef MSG_SEND_WATCHDOG
#define WD_TIMEOUT wdDeadline
#else
#define WD_TIMEOUT 0
#endif

static int proc_sendEx(u32 port, msg_t *msg, int interruptible)
{
	port_t *p;
	int err = EOK;
	kmsg_t kmsg;
	thread_t *sender;
	spinlock_ctx_t sc;
	int state = msg_rejected;
	int exiting = 0, detached = 0;
	msg_exitwait_t ew = { 0 };
#ifdef MSG_SEND_WATCHDOG
	/* DIAGNOSTIC (-DMSG_SEND_WATCHDOG=<seconds>): the `premain-hang` bisect ends
	 * in an open() that never returns, i.e. this round trip. kmsg.state says
	 * which half is stuck and nothing else can:
	 *   msg_waiting  -> the server never took the request off the port
	 *   msg_received -> the server took it and never responded
	 * One shot: it reports once and then goes back to waiting forever, so the
	 * only behaviour change is a single line. */
	time_t wdRaw, wdOffs, wdDeadline = 0;
#endif

	/* TODO - check if msg pointer belongs to user vm_map */
	if (msg == NULL) {
		return -EINVAL;
	}

	p = proc_portGet(port);
	if (p == NULL) {
		return -EINVAL;
	}

	sender = proc_current();

	hal_memcpy(&kmsg.msg, msg, sizeof(msg_t));
	kmsg.src = sender->process;
	kmsg.dst = NULL;
	kmsg.threads = NULL;
	kmsg.state = msg_waiting;
	kmsg.idlinkage.id = -1; /* No rid until it is received: matches no node of the rid tree */

	kmsg.msg.pid = (sender->process != NULL) ? process_getPid(sender->process) : 0;
	kmsg.msg.priority = sender->priority;

	msg_ipack(&kmsg);

#ifdef MSG_SEND_WATCHDOG
	/* Computed BEFORE the port spinlock: proc_gettime() takes a lock of its own
	 * and nesting it under p->spinlock is exactly the shape that has deadlocked
	 * this kernel before. */
	proc_gettime(&wdRaw, &wdOffs);
	wdDeadline = wdRaw + ((time_t)MSG_SEND_WATCHDOG * 1000LL * 1000LL);
#endif

	hal_spinlockSet(&p->spinlock, &sc);

	if (p->closed != 0) {
		err = -EINVAL;
	}
	else {
		trace_eventMsgSend(proc_getTid(sender), port, kmsg.msg.type, &kmsg);
		LIST_ADD(&p->kmessages, &kmsg);
		(void)proc_threadWakeup(&p->threads);

		state = kmsg.state;
		while ((state != msg_responded) && (state != msg_rejected)) {
			if (exiting != 0) {
				/* Told to exit: abandon the request if it is safe (msg_senderExiting()), or wait for its
				 * response in steps that time out */
				hal_spinlockClear(&p->spinlock, &sc);
				detached = msg_senderExiting(p, &kmsg, port, state, interruptible, &ew);
				hal_spinlockSet(&p->spinlock, &sc);

				state = kmsg.state;
				if ((state == msg_responded) || (state == msg_rejected)) {
					break;
				}
				if (detached != 0) {
					err = -EINTR;
					break;
				}
				(void)proc_threadWait(&kmsg.threads, &p->spinlock, ew.deadline, &sc);
				err = EOK; /* the end of a step is not an error */
			}
			else if ((state == msg_waiting) && (interruptible != 0)) {
				/* WD_TIMEOUT rather than upstream's 0: with MSG_SEND_WATCHDOG
				 * built in, a send that never gets a response has to come back
				 * so the watchdog below can report it. It is 0 when the watchdog
				 * is compiled out, i.e. identical to upstream then. */
				err = proc_threadWaitInterruptible(&kmsg.threads, &p->spinlock, WD_TIMEOUT, &sc);
			}
			else {
				/* A received request (or any request of an uninterruptible sender) is waited for until it is
				 * answered: the kmsg lives on this stack, and the receiver may hold it. A signal does not end
				 * this wait (it would return at once for every pending signal, with p->spinlock held), but an
				 * exit request does, so that this thread can abandon the request or say what it waits for. */
				err = proc_threadWaitKillable(&kmsg.threads, &p->spinlock, WD_TIMEOUT, &sc);
				if (err == -EINTR) {
					exiting = 1;
					err = EOK;
				}
			}

			state = kmsg.state;
#ifdef MSG_SEND_WATCHDOG
			if ((wdDeadline != 0) && (state != msg_responded) && (state != msg_rejected)) {
				int wdState = state;

				wdDeadline = 0; /* one shot; the wait is unbounded again from here */
				err = EOK;      /* our own deadline is not a real error */
				hal_spinlockClear(&p->spinlock, &sc);
				lib_printf("proc: SEND-WATCHDOG port=%u state=%d (%s)\n", port, wdState,
					(wdState == msg_waiting) ? "waiting: server never took it" :
						((wdState == msg_received) ? "received: server never responded" : "?"));
				hal_spinlockSet(&p->spinlock, &sc);
				state = kmsg.state; /* may have changed while we were unlocked */
			}
#endif
			if ((err != EOK) && (state == msg_waiting)) {
				LIST_REMOVE(&p->kmessages, &kmsg);
				break;
			}
		}

		switch (state) {
			case msg_responded:
				err = EOK; /* Don't report EINTR if we got the response already */
				break;
			case msg_rejected:
				err = -EINVAL;
				break;
			default:
				/* No action required */
				break;
		}
	}

	hal_spinlockClear(&p->spinlock, &sc);
	port_put(p, 0);

	/* A file written, truncated or changed, or a new file under an id that may have named another
	 * one: the pages the vm keeps of it are stale. Whatever the outcome -- a server can change a
	 * file and still fail the request. */
	vm_objectNotify(kmsg.msg.type, &kmsg.msg.oid, (state == msg_responded) ? &kmsg.msg.o.create.oid : NULL);

	if (err == EOK) {
		hal_memcpy(msg->o.raw, kmsg.msg.o.raw, sizeof(msg->o.raw));
		msg->o.err = kmsg.msg.o.err;

		/* If msg.o.data has been packed to msg.o.raw */
		if ((kmsg.msg.o.data >= (void *)kmsg.msg.o.raw) && (kmsg.msg.o.data < (void *)kmsg.msg.o.raw + sizeof(kmsg.msg.o.raw))) {
			hal_memcpy(msg->o.data, kmsg.msg.o.data, msg->o.size);
		}
	}

	return err;
}


int proc_send(u32 port, msg_t *msg)
{
	return proc_sendEx(port, msg, 1);
}


int proc_sendUninterruptible(u32 port, msg_t *msg)
{
	return proc_sendEx(port, msg, 0);
}


int proc_recv(u32 port, msg_t *msg, msg_rid_t *rid)
{
	port_t *p;
	kmsg_t *kmsg;
	int ipacked = 0, opacked = 0, err = EOK;
	msg_rid_t ret;
	spinlock_ctx_t sc;

	p = proc_portGet(port);
	if (p == NULL) {
		return -EINVAL;
	}

	hal_spinlockSet(&p->spinlock, &sc);

	while ((p->kmessages == NULL) && (p->closed == 0) && (err != -EINTR)) {
		err = proc_threadWaitInterruptible(&p->threads, &p->spinlock, 0, &sc);
	}

	kmsg = p->kmessages;

	if (p->closed != 0) {
		/* Port is being removed */
		if (kmsg != NULL) {
			kmsg->state = msg_rejected;
			LIST_REMOVE(&p->kmessages, kmsg);
			(void)proc_threadWakeup(&kmsg->threads);
		}

		err = -EINVAL;
	}
	else {
		if (err == EOK) {
			LIST_REMOVE(&p->kmessages, kmsg);
			kmsg->state = msg_received;
			trace_eventMsgRecv(port, kmsg, kmsg->msg.pid);
		}
	}
	hal_spinlockClear(&p->spinlock, &sc);

	if (err != EOK) {
		port_put(p, 0);
		return err;
	}

	kmsg->i.bvaddr = NULL;
	kmsg->i.boffs = 0;
	kmsg->i.w = NULL;
	kmsg->i.bp = NULL;
	kmsg->i.evaddr = NULL;
	kmsg->i.eoffs = 0;
	kmsg->i.ep = NULL;

	kmsg->o.bvaddr = NULL;
	kmsg->o.boffs = 0;
	kmsg->o.w = NULL;
	kmsg->o.bp = NULL;
	kmsg->o.evaddr = NULL;
	kmsg->o.eoffs = 0;
	kmsg->o.ep = NULL;

	kmsg->dst = proc_current()->process;
	kmsg->receiver = proc_getTid(proc_current());

	if ((kmsg->msg.i.data >= (void *)kmsg->msg.i.raw) && (kmsg->msg.i.data < (void *)kmsg->msg.i.raw + sizeof(kmsg->msg.i.raw))) {
		ipacked = 1;
	}

	/* Map data in receiver space */
	/* Don't map if msg is packed */
	if (ipacked == 0) {
		kmsg->msg.i.data = msg_map(0, kmsg, (void *)(ptr_t)kmsg->msg.i.data, kmsg->msg.i.size, kmsg->src, proc_current()->process);
	}

	opacked = msg_opack(kmsg);
	if (opacked == 0) {
		kmsg->msg.o.data = msg_map(1, kmsg, kmsg->msg.o.data, kmsg->msg.o.size, kmsg->src, proc_current()->process);
	}

	ret = -ENOMEM;
	if (((kmsg->msg.i.size == 0U) || (kmsg->msg.i.data != NULL)) && ((kmsg->msg.o.size == 0U) || (kmsg->msg.o.data != NULL))) {
		hal_memcpy(msg, &kmsg->msg, sizeof(*msg));

		if (ipacked != 0) {
			msg->i.data = msg->i.raw + (kmsg->msg.i.data - (void *)kmsg->msg.i.raw);
		}

		if (opacked != 0) {
			msg->o.data = msg->o.raw + (kmsg->msg.o.data - (void *)kmsg->msg.o.raw);
		}

		/* Publishing the rid hands kmsg over to proc_respond(), or back to an exiting sender, which may
		 * then leave (msg_detach()): it is not touched from here on */
		ret = proc_portRidAlloc(p, kmsg);
	}

	if (ret < 0) {
		msg_release(kmsg);

		hal_spinlockSet(&p->spinlock, &sc);
		kmsg->state = msg_rejected;
		(void)proc_threadWakeup(&kmsg->threads);
		hal_spinlockClear(&p->spinlock, &sc);

		port_put(p, 0);

		return -ENOMEM;
	}

	*rid = ret;

	port_put(p, 0);

	return EOK;
}


int proc_respond(u32 port, msg_t *msg, msg_rid_t rid)
{
	port_t *p;
	kmsg_t *kmsg;
	spinlock_ctx_t sc;

	p = proc_portGet(port);
	if (p == NULL) {
		return -EINVAL;
	}

	kmsg = proc_portRidGet(p, rid);
	if (kmsg == NULL) {
		port_put(p, 0);
		return -ENOENT;
	}

	/* Copy shadow pages */
	if (kmsg->i.bp != NULL) {
		hal_memcpy(kmsg->i.bvaddr + kmsg->i.boffs, kmsg->i.w + kmsg->i.boffs, (size_t)min(SIZE_PAGE - kmsg->i.boffs, kmsg->msg.i.size));
	}

	if (kmsg->i.eoffs != 0U) {
		hal_memcpy(kmsg->i.evaddr, kmsg->i.w + kmsg->i.boffs + kmsg->msg.i.size - kmsg->i.eoffs, (size_t)kmsg->i.eoffs);
	}

	if (kmsg->o.bp != NULL) {
		hal_memcpy(kmsg->o.bvaddr + kmsg->o.boffs, kmsg->o.w + kmsg->o.boffs, (size_t)min(SIZE_PAGE - kmsg->o.boffs, kmsg->msg.o.size));
	}

	if (kmsg->o.eoffs != 0U) {
		hal_memcpy(kmsg->o.evaddr, kmsg->o.w + kmsg->o.boffs + kmsg->msg.o.size - kmsg->o.eoffs, (size_t)kmsg->o.eoffs);
	}

	msg_release(kmsg);

	hal_memcpy(kmsg->msg.o.raw, msg->o.raw, sizeof(msg->o.raw));
	kmsg->msg.o.err = msg->o.err;

	hal_spinlockSet(&p->spinlock, &sc);
	trace_eventMsgRespond(port, kmsg);
	kmsg->state = msg_responded;
	kmsg->src = proc_current()->process;
	(void)proc_threadWakeup(&kmsg->threads);
	hal_spinlockClear(&p->spinlock, &sc);
	(void)hal_cpuReschedule(NULL, NULL);

	port_put(p, 0);

	return EOK;
}


void proc_msgRejectPending(port_t *p, const process_t *receiver)
{
	kmsg_t *kmsg, *rejected = NULL;
	idnode_t *n;
	spinlock_ctx_t sc;

	/* Close the port and fail what nobody has received: no receiver is left to do it */
	hal_spinlockSet(&p->spinlock, &sc);
	p->closed = 1;
	while ((kmsg = p->kmessages) != NULL) {
		LIST_REMOVE(&p->kmessages, kmsg);
		kmsg->state = msg_rejected;
		(void)proc_threadWakeup(&kmsg->threads);
	}
	hal_spinlockClear(&p->spinlock, &sc);

	if (receiver == NULL) {
		return;
	}

	/*
	 * Received requests are reachable only through their rids. Take over those of the dead
	 * receiver: their payload windows went away with its address space, so only the kernel's
	 * shadow pages are left to release. A request taken by another, live process is left to
	 * it: its window still maps the sender's pages. kmsg->next is free once received.
	 */
	(void)proc_lockSet(&p->lock);
	for (n = lib_idtreeMinimum(p->rid.root); n != NULL; n = lib_idtreeNext(&n->linkage)) {
		kmsg = lib_treeof(kmsg_t, idlinkage, n);
		if (kmsg->dst == receiver) {
			LIST_ADD(&rejected, kmsg);
		}
	}

	kmsg = rejected;
	if (kmsg != NULL) {
		do {
			lib_idtreeRemove(&p->rid, &kmsg->idlinkage);
			kmsg = kmsg->next;
		} while (kmsg != rejected);
	}
	(void)proc_lockClear(&p->lock);

	/* The kmsg lives on its sender's stack: it must not be touched once the sender is woken */
	while ((kmsg = rejected) != NULL) {
		LIST_REMOVE(&rejected, kmsg);
		msg_releaseShadow(&kmsg->i);
		msg_releaseShadow(&kmsg->o);

		hal_spinlockSet(&p->spinlock, &sc);
		kmsg->state = msg_rejected;
		(void)proc_threadWakeup(&kmsg->threads);
		hal_spinlockClear(&p->spinlock, &sc);
	}
}


void _msg_init(vm_map_t *kmap, vm_object_t *kernel)
{
	msg_common.kmap = kmap;
	msg_common.kernel = kernel;
}
