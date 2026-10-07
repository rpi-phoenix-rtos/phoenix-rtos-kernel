/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * UNIX socket data channel
 *
 * Copyright 2026 Phoenix Systems
 * Author: Ziemowit Leszczynski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "include/errno.h"
#include "lib/lib.h"
#include "proc/proc.h"
#include "vm/vm.h"

#include "uchannel.h"
#include "pollwake.h"


/*
 * A frame is stored as its length followed by its bytes. The top bit of the
 * length word says that a descriptor pack travels with the frame: the packs of
 * a framed channel are queued in frame order, one per marked frame, so the
 * reader of a marked frame takes exactly the oldest pack. A frame can never be
 * that long - it has to fit the ring - so the bit is free.
 */
#define UCHANNEL_FRAME_FDS ((size_t)1U << ((sizeof(size_t) * 8U) - 1U))


/*
 * Readiness-woken poll() for AF_UNIX. usocket_poll() is a level-triggered
 * snapshot that registers nothing, so without a wake-up a poller could only
 * re-check every POLL_INTERVAL, and every libxcb or WebKit IPC round trip would
 * pay for it. Every channel state change therefore wakes the pollwake waiters
 * of the poll() sets that hold an AF_UNIX socket. Each such poller sleeps on a
 * queue of its own, listed before its first query (posix/pollwake.h), so a
 * change that lands between a poller's query and its sleep is not lost.
 *
 * It is called inside proc_lockSet() on the channel, where taking the pollwake
 * spinlock is the allowed order.
 */
void uchannel_pollNotify(void)
{
	pollwake_notifyUnix();
}


/*
 * A pipe's channel wakes only the poll() sets that watch its oid: a pipe is
 * not an AF_UNIX socket, and waking every AF_UNIX poller in the system on each
 * byte of a GLib wake-up pipe would be a thundering herd.
 */
static void _uchannel_notify(const uchannel_t *ch)
{
	if (ch->pollTargeted != 0U) {
		pollwake_notify(&ch->pollOid);
	}
	else {
		pollwake_notifyUnix();
	}
}


size_t uchannel_roundSize(size_t size)
{
	if ((size != 0U) && ((size & (size - 1U)) != 0U)) {
		size = (size_t)1U << (hal_cpuGetLastBit(size) + 1U);
	}

	return size;
}


uchannel_t *uchannel_alloc(size_t size, int framed)
{
	uchannel_t *ch;
	void *data;

	ch = vm_kmalloc(sizeof(uchannel_t));
	if (ch == NULL) {
		return NULL;
	}

	data = vm_kmalloc(size);
	if (data == NULL) {
		vm_kfree(ch);
		return NULL;
	}

	if (proc_lockInit(&ch->lock, &proc_lockAttrDefault, "unix.channel") < 0) {
		vm_kfree(data);
		vm_kfree(ch);
		return NULL;
	}

	ch->refs = 1;
	ch->framed = (framed != 0) ? 1U : 0U;
	ch->pollTargeted = 0;
	ch->atomic = 0;
	ch->maxSize = 0; /* a socket's ring never grows by itself */
	hal_memset(&ch->pollOid, 0, sizeof(ch->pollOid));
	ch->flags = 0;
	ch->fdpacks = NULL;
	ch->rxwait = NULL;
	ch->txwait = NULL;
	_cbuffer_init(&ch->buffer, data, size);

	return ch;
}


uchannel_t *uchannel_allocStream(size_t size, size_t maxSize, size_t atomic, const oid_t *pollOid)
{
	uchannel_t *ch = uchannel_alloc(size, 0);

	if (ch != NULL) {
		/* not shared yet, so no lock is needed */
		ch->maxSize = max(size, maxSize);
		ch->atomic = atomic;
		hal_memcpy(&ch->pollOid, pollOid, sizeof(ch->pollOid));
		ch->pollTargeted = 1;
	}

	return ch;
}


uchannel_t *uchannel_ref(uchannel_t *ch)
{
	if (ch != NULL) {
		(void)lib_atomicIncrement(&ch->refs);
	}

	return ch;
}


void uchannel_put(uchannel_t *ch)
{
	fdpack_t *packs;

	if (ch == NULL) {
		return;
	}

	if (lib_atomicDecrement(&ch->refs) != 0) {
		return;
	}

	/*
	 * The last reference is gone, so the channel is unreachable and no lock is
	 * needed. The descriptors are discarded after the channel itself is freed,
	 * as fdpass_discard() reaches back into the file descriptor table.
	 */
	packs = ch->fdpacks;

	(void)proc_lockDone(&ch->lock);
	vm_kfree(ch->buffer.data);
	vm_kfree(ch);

	if (packs != NULL) {
		fdpass_discard(&packs);
	}
}


/*
 * Grows the ring of a stream channel so that `want` more bytes fit, up to
 * maxSize. Called and returns with the lock held, but drops it around the
 * allocation and the free. Returns EOK when the ring is now bigger than
 * `oldSize` (grown here or by a concurrent writer), so the caller re-checks
 * everything it has seen, or -ENOMEM.
 */
static int _uchannel_grow(uchannel_t *ch, size_t oldSize, size_t want)
{
	cbuffer_t old;
	size_t size, avail, first;
	void *data, *unused;

	size = uchannel_roundSize(min(ch->maxSize, _cbuffer_avail(&ch->buffer) + want));
	size = max(size, oldSize * 2U);
	size = min(size, ch->maxSize);

	(void)proc_lockClear(&ch->lock);
	data = vm_kmalloc(size);
	(void)proc_lockSet(&ch->lock);

	if (ch->buffer.sz > oldSize) {
		/* a concurrent writer has grown it meanwhile */
		unused = data;
	}
	else if (data == NULL) {
		return -ENOMEM;
	}
	else {
		/* the ring never shrinks, so everything queued fits the new one */
		avail = _cbuffer_avail(&ch->buffer);
		hal_memcpy(&old, &ch->buffer, sizeof(old));
		_cbuffer_init(&ch->buffer, data, size);
		if (avail > 0U) {
			first = min(avail, old.sz - old.r);
			(void)_cbuffer_write(&ch->buffer, (const char *)old.data + old.r, first);
			if (avail > first) {
				(void)_cbuffer_write(&ch->buffer, old.data, avail - first);
			}
		}
		unused = old.data;
	}

	if (unused != NULL) {
		(void)proc_lockClear(&ch->lock);
		vm_kfree(unused);
		(void)proc_lockSet(&ch->lock);
	}

	return EOK;
}


ssize_t uchannel_write(uchannel_t *ch, const void *buf, size_t len, unsigned int flags, fdpack_t *fdpack)
{
	ssize_t ret = 0;
	size_t done = 0, chunk, hdr, room, need;
	int err, whole;

	/* an atomic write waits until all of it fits, and only then writes it */
	whole = (((flags & UCHANNEL_OP_ATOMIC) != 0U) && (ch->framed == 0U) && (len <= ch->atomic)) ? 1 : 0;

	(void)proc_lockSet(&ch->lock);

	for (;;) {
		if ((ch->flags & (UCHANNEL_SHUT_RD | UCHANNEL_SHUT_WR)) != 0U) {
			/* bytes already handed over are reported, the error by the next call */
			ret = (done > 0U) ? (ssize_t)done : -EPIPE;
			break;
		}

		if (len == 0U) {
			ret = 0;
			break;
		}

		if (ch->framed == 0U) {
			room = _cbuffer_free(&ch->buffer);
			need = len - done;
			if ((room < need) && (ch->buffer.sz < ch->maxSize)) {
				/* grow before waiting; with the ring at maxSize, wait as usual */
				if (_uchannel_grow(ch, ch->buffer.sz, need) == EOK) {
					continue;
				}
			}

			if ((whole != 0) && (room < len)) {
				chunk = 0;
			}
			else {
				chunk = _cbuffer_write(&ch->buffer, (const char *)buf + done, need);
			}
			if (chunk > 0U) {
				if ((done == 0U) && (fdpack != NULL)) {
					/* the descriptors travel with the first byte of the write */
					LIST_ADD(&ch->fdpacks, fdpack);
				}
				done += chunk;
				(void)proc_threadBroadcast(&ch->rxwait);
				_uchannel_notify(ch);
			}

			if (done == len) {
				ret = (ssize_t)done;
				break;
			}
		}
		else if (len > (ch->buffer.sz - sizeof(len))) {
			ret = -EMSGSIZE;
			break;
		}
		else if (_cbuffer_free(&ch->buffer) >= (len + sizeof(len))) {
			hdr = len;
			if (fdpack != NULL) {
				/* the descriptors belong to this frame and to no other */
				hdr |= UCHANNEL_FRAME_FDS;
				LIST_ADD(&ch->fdpacks, fdpack);
			}
			(void)_cbuffer_write(&ch->buffer, &hdr, sizeof(hdr));
			(void)_cbuffer_write(&ch->buffer, buf, len);
			(void)proc_threadBroadcast(&ch->rxwait);
			_uchannel_notify(ch);
			ret = (ssize_t)len;
			break;
		}
		else {
			/* not enough room for the whole frame */
		}

		if ((flags & UCHANNEL_OP_NONBLOCK) != 0U) {
			ret = (done > 0U) ? (ssize_t)done : -EWOULDBLOCK;
			break;
		}

		err = proc_lockWait(&ch->txwait, &ch->lock, 0);
		if (err == -EINTR) {
			/* the lock has not been reacquired */
			return (done > 0U) ? (ssize_t)done : -EINTR;
		}
		if (err < 0) {
			ret = (done > 0U) ? (ssize_t)done : (ssize_t)err;
			break;
		}
	}

	(void)proc_lockClear(&ch->lock);

	return ret;
}


static void _uchannel_takePacks(uchannel_t *ch, fdpack_t **packs)
{
	*packs = ch->fdpacks;
	ch->fdpacks = NULL;
}


ssize_t uchannel_read(uchannel_t *ch, void *buf, size_t len, unsigned int flags, fdpack_t **packs)
{
	ssize_t ret = 0;
	size_t rlen = 0;
	fdpack_t *framePack = NULL, *pack;
	int err, hasFds;

	if (packs != NULL) {
		*packs = NULL;
	}

	(void)proc_lockSet(&ch->lock);

	for (;;) {
		if (len == 0U) {
			/*
			 * A zero-length read waits for data but takes none of it.
			 * Note that read(len = 0) never gets here, as posix_read()
			 * answers it with 0 straight away.
			 */
			ret = 0;
			if (_cbuffer_avail(&ch->buffer) > 0U) {
				break;
			}
		}
		else if (ch->framed == 0U) {
			if ((flags & UCHANNEL_OP_PEEK) != 0U) {
				ret = (ssize_t)_cbuffer_peek(&ch->buffer, buf, len);
			}
			else {
				ret = (ssize_t)_cbuffer_read(&ch->buffer, buf, len);
			}
		}
		else if (_cbuffer_avail(&ch->buffer) > sizeof(rlen)) {
			(void)_cbuffer_peek(&ch->buffer, &rlen, sizeof(rlen));
			hasFds = ((rlen & UCHANNEL_FRAME_FDS) != 0U) ? 1 : 0;
			rlen &= ~UCHANNEL_FRAME_FDS;
			ret = (ssize_t)min(len, rlen);

			if ((flags & UCHANNEL_OP_PEEK) != 0U) {
				(void)_cbuffer_peekAt(&ch->buffer, sizeof(rlen), buf, (size_t)ret);
			}
			else {
				(void)_cbuffer_discard(&ch->buffer, sizeof(rlen));
				(void)_cbuffer_read(&ch->buffer, buf, (size_t)ret);

				if (rlen > (size_t)ret) {
					/* the rest of a truncated frame is dropped */
					(void)_cbuffer_discard(&ch->buffer, rlen - (size_t)ret);
				}

				if ((hasFds != 0) && (ch->fdpacks != NULL)) {
					/*
					 * The oldest pack is this frame's. Leaving it queued would hand
					 * it to the next reader of a later frame, which is how a reader
					 * that drains two frames with descriptors got both sets of
					 * descriptors with the first and none with the second.
					 * ret > 0 here (len > 0 and no frame is empty), so the pack is
					 * either handed out or closed below.
					 */
					pack = ch->fdpacks;
					LIST_REMOVE(&ch->fdpacks, pack);
					/* a list of its own: LIST_REMOVE() leaves the links zeroed */
					LIST_ADD(&framePack, pack);
				}
			}
		}
		else {
			/* no complete frame */
		}

		if (ret > 0) {
			if ((flags & UCHANNEL_OP_PEEK) == 0U) {
				if (ch->framed != 0U) {
					if (packs != NULL) {
						*packs = framePack;
						framePack = NULL;
					}
				}
				else if (packs != NULL) {
					_uchannel_takePacks(ch, packs);
				}
				else {
					/* No action */
				}
				(void)proc_threadBroadcast(&ch->txwait);
				_uchannel_notify(ch);
			}
			break;
		}

		/*
		 * EOS, but only once everything queued has been delivered.
		 * Data written before a shutdown must still be readable.
		 */
		if ((ch->flags & (UCHANNEL_SHUT_WR | UCHANNEL_SHUT_RD)) != 0U) {
			ret = 0;
			break;
		}

		if ((flags & UCHANNEL_OP_NONBLOCK) != 0U) {
			ret = -EWOULDBLOCK;
			break;
		}

		err = proc_lockWait(&ch->rxwait, &ch->lock, 0);
		if (err == -EINTR) {
			/* the lock has not been reacquired */
			return -EINTR;
		}
		if (err < 0) {
			ret = (ssize_t)err;
			break;
		}
	}

	(void)proc_lockClear(&ch->lock);

	if (framePack != NULL) {
		/*
		 * The frame was read without room for its descriptors, so they are
		 * closed, as on Linux. With no lock held: fdpass_discard() reaches back
		 * into the file descriptor table.
		 */
		fdpass_discard(&framePack);
	}

	return ret;
}


void uchannel_returnPacks(uchannel_t *ch, fdpack_t **packs)
{
	fdpack_t *head, *pack;

	if (*packs == NULL) {
		return;
	}

	(void)proc_lockSet(&ch->lock);

	if (ch->fdpacks == NULL) {
		ch->fdpacks = *packs;
	}
	else {
		/*
		 * The list is circular and `fdpacks` points at its oldest entry, so
		 * appending the leftovers and then moving the head onto the first of
		 * them puts them back in front of anything queued in the meantime.
		 */
		head = *packs;
		do {
			pack = *packs;
			LIST_REMOVE(packs, pack);
			LIST_ADD(&ch->fdpacks, pack);
		} while (*packs != NULL);

		ch->fdpacks = head;
	}

	*packs = NULL;

	(void)proc_lockClear(&ch->lock);
}


/*
 * Discards the descriptor packs queued in the channel. Called once the endpoint
 * that reads the channel is gone, at which point nothing can ever deliver them.
 * No lock is held over fdpass_discard(), which reaches back into the file
 * descriptor table.
 */
void uchannel_discardPacks(uchannel_t *ch)
{
	fdpack_t *packs;

	(void)proc_lockSet(&ch->lock);
	_uchannel_takePacks(ch, &packs);
	(void)proc_lockClear(&ch->lock);

	if (packs != NULL) {
		fdpass_discard(&packs);
	}
}


void uchannel_shutWr(uchannel_t *ch)
{
	(void)proc_lockSet(&ch->lock);

	ch->flags |= UCHANNEL_SHUT_WR;

	/* readers see EOS, writers (of a half-closed local end) EPIPE */
	(void)proc_threadBroadcast(&ch->rxwait);
	(void)proc_threadBroadcast(&ch->txwait);
	_uchannel_notify(ch);

	(void)proc_lockClear(&ch->lock);
}


void uchannel_shutRd(uchannel_t *ch)
{
	(void)proc_lockSet(&ch->lock);

	ch->flags |= UCHANNEL_SHUT_RD;

	(void)proc_threadBroadcast(&ch->txwait);
	(void)proc_threadBroadcast(&ch->rxwait);
	_uchannel_notify(ch);

	(void)proc_lockClear(&ch->lock);
}


unsigned int uchannel_pollRd(uchannel_t *ch)
{
	unsigned int events = 0;

	(void)proc_lockSet(&ch->lock);

	if ((_cbuffer_avail(&ch->buffer) > 0U) || ((ch->flags & (UCHANNEL_SHUT_WR | UCHANNEL_SHUT_RD)) != 0U)) {
		events |= UCHANNEL_EV_IN;
	}

	if ((ch->flags & (UCHANNEL_SHUT_WR | UCHANNEL_SHUT_RD)) != 0U) {
		events |= UCHANNEL_EV_SHUT;
	}

	if (_cbuffer_avail(&ch->buffer) > 0U) {
		events |= UCHANNEL_EV_DATA;
	}

	(void)proc_lockClear(&ch->lock);

	return events;
}


unsigned int uchannel_pollWr(uchannel_t *ch)
{
	unsigned int events = 0;
	size_t free;

	(void)proc_lockSet(&ch->lock);

	free = _cbuffer_free(&ch->buffer);
	if (ch->framed == 0U) {
		/*
		 * A byte stream takes a partial write, but an atomic one needs room for
		 * all of it: report EV_OUT only when the largest atomic write fits (or
		 * the ring can still grow to make room), or a writer that waits for
		 * POLLOUT after EAGAIN would spin. Linux reports a pipe writable with a
		 * whole page free for the same reason.
		 */
		if (((free > 0U) && (free >= ch->atomic)) || (ch->buffer.sz < ch->maxSize)) {
			events |= UCHANNEL_EV_OUT;
		}
	}
	else if (free >= (ch->buffer.sz - (ch->buffer.sz / 4U))) {
		/*
		 * A frame goes in whole or not at all, so "one byte free" would report
		 * POLLOUT for a message that still does not fit: a sender that waits
		 * for POLLOUT after EAGAIN (WebKit's IPC) would spin until the reader
		 * caught up. Writable while at most a quarter of the ring is in use,
		 * as Linux does for these sockets: any frame of up to three quarters
		 * of the ring, length word included, then fits.
		 */
		events |= UCHANNEL_EV_OUT;
	}
	else {
		/* No action required */
	}

	if ((ch->flags & (UCHANNEL_SHUT_WR | UCHANNEL_SHUT_RD)) != 0U) {
		events |= UCHANNEL_EV_SHUT;
	}

	(void)proc_lockClear(&ch->lock);

	return events;
}


int uchannel_resize(uchannel_t *ch, size_t size)
{
	void *data;
	cbuffer_t old;
	size_t avail, first;
	fdpack_t *dropped = NULL;

	data = vm_kmalloc(size);
	if (data == NULL) {
		return -ENOMEM;
	}

	(void)proc_lockSet(&ch->lock);

	avail = _cbuffer_avail(&ch->buffer);
	old = ch->buffer;
	_cbuffer_init(&ch->buffer, data, size);

	if (avail <= size) {
		/* copy the buffered bytes over in order */
		if (avail > 0U) {
			/* TODO: add _cbuffer_copy() */
			first = min(avail, old.sz - old.r);
			(void)_cbuffer_write(&ch->buffer, (const char *)old.data + old.r, first);
			if (avail > first) {
				(void)_cbuffer_write(&ch->buffer, old.data, avail - first);
			}
		}
	}
	else {
		/*
		 * FIXME: the buffered data does not fit the new ring and is dropped
		 * whole, so that a frame is never left half present. The sender's
		 * send() has already reported those bytes as accepted and the reader
		 * has no way to tell they went missing - a byte stream loses a piece
		 * out of its middle, a framed socket loses entire records.
		 */
		if (ch->framed != 0U) {
			/* the descriptors of the dropped frames must not attach to later ones */
			_uchannel_takePacks(ch, &dropped);
		}
	}

	(void)proc_threadBroadcast(&ch->txwait);
	_uchannel_notify(ch);

	(void)proc_lockClear(&ch->lock);

	vm_kfree(old.data);

	if (dropped != NULL) {
		fdpass_discard(&dropped);
	}

	return 0;
}


size_t uchannel_size(uchannel_t *ch)
{
	size_t size;

	(void)proc_lockSet(&ch->lock);
	size = ch->buffer.sz;
	(void)proc_lockClear(&ch->lock);

	return size;
}


size_t uchannel_avail(uchannel_t *ch)
{
	size_t avail;

	(void)proc_lockSet(&ch->lock);
	avail = (ch->framed == 0U) ? _cbuffer_avail(&ch->buffer) : 0U;
	(void)proc_lockClear(&ch->lock);

	return avail;
}
