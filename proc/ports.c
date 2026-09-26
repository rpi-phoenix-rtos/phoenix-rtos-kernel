/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Ports
 *
 * Copyright 2017, 2018, 2023 Phoenix Systems
 * Author: Jakub Sejdak, Pawel Pisarczyk, Aleksander Kaminski, Jan Sikorski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ports.h"
#include "lib/lib.h"


static struct {
	idtree_t tree;
	lock_t port_lock;
} port_common;


msg_rid_t proc_portRidAlloc(port_t *p, kmsg_t *kmsg)
{
	msg_rid_t ret;

	(void)proc_lockSet(&p->lock);
	ret = lib_idtreeAlloc(&p->rid, &kmsg->idlinkage, 0);
	(void)proc_lockClear(&p->lock);

	return ret;
}


kmsg_t *proc_portRidGet(port_t *p, msg_rid_t rid)
{
	kmsg_t *kmsg;

	(void)proc_lockSet(&p->lock);

	kmsg = lib_treeof(kmsg_t, idlinkage, lib_idtreeFind(&p->rid, rid));
	if (kmsg != NULL) {
		lib_idtreeRemove(&p->rid, &kmsg->idlinkage);
	}

	(void)proc_lockClear(&p->lock);

	return kmsg;
}


port_t *proc_portGet(u32 id)
{
	port_t *port;
	spinlock_ctx_t sc;

	if (id > MAX_ID) {
		return NULL;
	}

	(void)proc_lockSet(&port_common.port_lock);
	port = lib_treeof(port_t, linkage, lib_idtreeFind(&port_common.tree, (int)id));
	if (port != NULL) {
		hal_spinlockSet(&port->spinlock, &sc);
		port->refs++;
		hal_spinlockClear(&port->spinlock, &sc);
	}
	(void)proc_lockClear(&port_common.port_lock);

	return port;
}


void port_put(port_t *p, int destroy)
{
	spinlock_ctx_t sc;

	(void)proc_lockSet(&port_common.port_lock);
	hal_spinlockSet(&p->spinlock, &sc);
	p->refs--;

	if (destroy != 0) {
		p->closed = 1;
	}

	if (p->refs != 0) {
		if (destroy != 0) {
			/* Wake receivers up */
			(void)proc_threadBroadcast(&p->threads);
		}

		hal_spinlockClear(&p->spinlock, &sc);
		(void)proc_lockClear(&port_common.port_lock);
		return;
	}

	hal_spinlockClear(&p->spinlock, &sc);
	lib_idtreeRemove(&port_common.tree, &p->linkage);
	(void)proc_lockClear(&port_common.port_lock);

	/* Ports on an owner's list hold the owner's reference, so a port reaching zero references
	 * normally has no owner left. A kernel port has none from the start. */
	if (p->owner != NULL) {
		(void)proc_lockSet(&p->owner->lock);
		if (p->next != NULL) {
			LIST_REMOVE(&p->owner->ports, p);
		}
		(void)proc_lockClear(&p->owner->lock);
	}

	(void)proc_lockDone(&p->lock);
	hal_spinlockDestroy(&p->spinlock);
	vm_kfree(p);
}


int proc_portCreate(u32 *id)
{
	port_t *port;
	thread_t *curr = proc_current();
	process_t *proc = (curr == NULL) ? NULL : curr->process;

	port = vm_kmalloc(sizeof(port_t));
	if (port == NULL) {
		return -ENOMEM;
	}

	(void)proc_lockSet(&port_common.port_lock);
	if (lib_idtreeAlloc(&port_common.tree, &port->linkage, 0) < 0) {
		(void)proc_lockClear(&port_common.port_lock);
		vm_kfree(port);
		return -ENOMEM;
	}

	port->kmessages = NULL;
	hal_spinlockCreate(&port->spinlock, "port.spinlock");

	lib_idtreeInit(&port->rid);
	(void)proc_lockInit(&port->lock, &proc_lockAttrDefault, "port.rid");

	port->threads = NULL;
	port->current = NULL;
	port->refs = 1;
	port->closed = 0;

	*id = (u32)port->linkage.id;
	port->owner = proc;
	(void)proc_lockClear(&port_common.port_lock);

	if (proc != NULL) {
		(void)proc_lockSet(&proc->lock);
		LIST_ADD((&proc->ports), port);
		(void)proc_lockClear(&proc->lock);
	}

	return EOK;
}


/*
 * Takes the port off the list of owner, as read by the caller. The owner's reference goes
 * with the list entry, so only the caller that unlinks the port may drop it: returns 1 if
 * that is this caller. The owner is forgotten too, because the port can outlive it (see
 * port_put()). A kernel port (owner NULL) is on no list.
 */
static int port_disown(port_t *p, process_t *owner)
{
	int ret = 1;

	if (owner != NULL) {
		(void)proc_lockSet(&owner->lock);
		if ((p->owner == owner) && (p->next != NULL)) {
			LIST_REMOVE(&owner->ports, p);
			p->owner = NULL;
		}
		else {
			ret = 0;
		}
		(void)proc_lockClear(&owner->lock);
	}

	return ret;
}


void proc_portDestroy(u32 port)
{
	port_t *p = proc_portGet(port);
	thread_t *curr = proc_current();
	process_t *proc = (curr == NULL) ? NULL : curr->process;
	process_t *owner;
	int owned = 0;

	if (p == NULL) {
		return;
	}

	owner = p->owner;
	if ((p->closed == 0) && ((proc == NULL) || (owner == proc))) {
		owned = port_disown(p, owner);
	}

	port_put(p, 0);
	if (owned != 0) {
		port_put(p, 1);
	}
}


void proc_portsDestroy(process_t *proc)
{
	port_t *p;

	for (;;) {
		(void)proc_lockSet(&proc->lock);
		p = proc->ports;
		if (p == NULL) {
			(void)proc_lockClear(&proc->lock);
			break;
		}
		LIST_REMOVE(&proc->ports, p);
		p->owner = NULL;
		(void)proc_lockClear(&proc->lock);
		port_put(p, 1);
	}
}


void _port_init(void)
{
	lib_idtreeInit(&port_common.tree);
	(void)proc_lockInit(&port_common.port_lock, &proc_lockAttrDefault, "port.common");
}
