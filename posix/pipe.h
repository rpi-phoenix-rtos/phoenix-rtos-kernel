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

#ifndef _PH_POSIX_PIPE_H_
#define _PH_POSIX_PIPE_H_

#include "hal/hal.h"
#include "proc/proc.h"
#include "posix.h"
#include "uchannel.h"


/*
 * An anonymous pipe is one byte-stream uchannel_t, shared by the open files of
 * its two ends. Each end holds a channel reference; the end an open file is
 * comes from its access mode (O_RDONLY or O_WRONLY), which never changes.
 *
 * The open file is the POSIX "open file description": fork(), dup() and
 * SCM_RIGHTS share it, so the last close of the read end's description is the
 * moment there are no readers left, and the same for the writers. That is when
 * the channel is shut in that direction - the reader then sees end-of-file once
 * the ring is drained, the writer EPIPE.
 *
 * Writes of up to PIPE_ATOMIC bytes are atomic. A non-blocking one either fits
 * whole or fails with EAGAIN, and POLLOUT is reported only when it would fit.
 *
 * The pipe's oid (POSIX_PORT_PIPE, a unique id) names no server: it is what
 * fstat() reports as the inode, and the key poll() sets watch to be woken by
 * the channel's state changes (posix/pollwake.h).
 */

/*
 * The atomic write size, the PIPE_BUF of these pipes. Linux's value (one page),
 * above the POSIX minimum of 512 (_POSIX_PIPE_BUF).
 */
#define PIPE_ATOMIC 4096U


int pipe_create(uchannel_t **ch, oid_t *oid);


/* `status` is the open file's: its access mode picks the end, O_NONBLOCK applies */
ssize_t pipe_read(uchannel_t *ch, void *buf, size_t len, unsigned int status);


ssize_t pipe_write(uchannel_t *ch, const void *buf, size_t len, unsigned int status);


int pipe_poll(uchannel_t *ch, unsigned int status, unsigned short events);


/* Bytes queued in the pipe (FIONREAD) */
size_t pipe_avail(uchannel_t *ch);


/* Drops the end's reference after shutting its direction down: the last close of that end */
void pipe_close(uchannel_t *ch, unsigned int status);


#endif
