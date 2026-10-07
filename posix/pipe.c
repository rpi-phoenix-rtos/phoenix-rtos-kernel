/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * POSIX-compatibility module, anonymous pipes
 *
 * Copyright 2026 Phoenix Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "include/errno.h"
#include "lib/lib.h"

#include "posix_private.h"
#include "pipe.h"


/*
 * The ring starts at one atomic write and grows on demand while the reader
 * falls behind, so the many idle pipes (every GLib main context has a wake-up
 * pipe) cost 4 kB each, as they did in posixsrv, while a bulk transfer gets a
 * 64 kB ring - Linux's default pipe capacity. A target without an MMU keeps
 * the small ring.
 */
#define PIPE_SIZE_MIN PIPE_ATOMIC
#ifdef NOMMU
#define PIPE_SIZE_MAX PIPE_SIZE_MIN
#else
#define PIPE_SIZE_MAX (64U * 1024U)
#endif


static struct {
	u32 nextId; /* atomic */
} pipe_common;


static unsigned int pipe_opFlags(unsigned int status)
{
	return ((status & O_NONBLOCK) != 0U) ? UCHANNEL_OP_NONBLOCK : 0U;
}


int pipe_create(uchannel_t **ch, oid_t *oid)
{
	oid->port = POSIX_PORT_PIPE;
	/* only an identity (fstat's st_ino, a poll wake-up key), so a wrap is harmless */
	oid->id = __atomic_add_fetch(&pipe_common.nextId, 1U, __ATOMIC_RELAXED);

	*ch = uchannel_allocStream(PIPE_SIZE_MIN, PIPE_SIZE_MAX, PIPE_ATOMIC, oid);

	return (*ch != NULL) ? EOK : -ENOMEM;
}


ssize_t pipe_read(uchannel_t *ch, void *buf, size_t len, unsigned int status)
{
	if (len == 0U) {
		/* a zero-length read returns at once; uchannel_read() would wait for data */
		return 0;
	}

	return uchannel_read(ch, buf, len, pipe_opFlags(status), NULL);
}


ssize_t pipe_write(uchannel_t *ch, const void *buf, size_t len, unsigned int status)
{
	if (len == 0U) {
		/* as on Linux: no EPIPE (nor SIGPIPE) for a write of nothing */
		return 0;
	}

	return uchannel_write(ch, buf, len, pipe_opFlags(status) | UCHANNEL_OP_ATOMIC, NULL);
}


int pipe_poll(uchannel_t *ch, unsigned int status, unsigned short events)
{
	unsigned int ev, revents = 0;

	/*
	 * Each end sees only the other end's shutdown - its own is its last close -
	 * so EV_SHUT is unambiguous. As on Linux: the reader gets POLLHUP once the
	 * writers are gone (with POLLIN while bytes are left to read), the writer
	 * POLLERR once the readers are gone.
	 */
	if ((status & O_WRONLY) != 0U) {
		ev = uchannel_pollWr(ch);
		if ((ev & UCHANNEL_EV_OUT) != 0U) {
			revents |= (unsigned int)events & (POLLOUT | POLLWRNORM);
		}
		if ((ev & UCHANNEL_EV_SHUT) != 0U) {
			revents |= POLLERR;
		}
	}
	else {
		ev = uchannel_pollRd(ch);
		if ((ev & UCHANNEL_EV_DATA) != 0U) {
			revents |= (unsigned int)events & (POLLIN | POLLRDNORM);
		}
		if ((ev & UCHANNEL_EV_SHUT) != 0U) {
			revents |= POLLHUP;
		}
	}

	return (int)revents;
}


size_t pipe_avail(uchannel_t *ch)
{
	return uchannel_avail(ch);
}


void pipe_close(uchannel_t *ch, unsigned int status)
{
	if ((status & O_WRONLY) != 0U) {
		uchannel_shutWr(ch);
	}
	else {
		uchannel_shutRd(ch);
	}

	uchannel_put(ch);
}
