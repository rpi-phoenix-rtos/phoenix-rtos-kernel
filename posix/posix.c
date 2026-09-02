/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * POSIX-compatibility module
 *
 * Copyright 2018, 2023 Phoenix Systems
 * Author: Jan Sikorski, Michal Miroslaw, Aleksander Kaminski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "include/errno.h"
#include "include/events.h"
#include "include/file.h"
#include "include/ioctl.h"
#include "include/limits.h"
#include "include/posix-fcntl.h"
#include "include/posix-wait.h"
#include "include/signal.h"
#include "include/sockdefs.h"

#include "proc/proc.h"

#include "posix_private.h"
#include "lib/lib.h"

#define MAX_FD_COUNT     1024
#define INITIAL_FD_COUNT 32

#if 0 /* Debug */
#define TRACE(str, ...) lib_printf("posix %x: " str "\n", proc_current()->process->id, ##__VA_ARGS__)
#else
#define TRACE(str, ...)
#endif

/*
 * Fallback re-check granularity (us) for poll()/select(). AF_UNIX fds are now
 * readiness-woken (posix_poll blocks on the unix poll queue via unix_pollWait,
 * woken the instant a socket changes state), so this interval only bounds the
 * latency for fds whose readiness comes from a remote server over mtGetAttr
 * (network sockets, devices), which has no readiness-wakeup yet. It is also the
 * safety-net timeout behind the AF_UNIX wait, so a missed notify degrades to
 * this latency rather than a hang. 20 ms: responsive enough for remote-fd polls
 * without the ~500Hz spin a sub-ms value would impose on every blocked poller.
 */
#define POLL_INTERVAL 20000


/* NOTE: socket/port ids are limited to 32-bits, hence possible downcast of oid.id from id_t to unsigned int */


typedef struct {
	oid_t oid;
	unsigned int flags;
	unsigned short types;
} evsub_t;


typedef struct _event_t {
	oid_t oid;
	unsigned int type;

	unsigned int flags;
	unsigned int count;
	unsigned int data;
} event_t;


/* POSIX advisory record lock (fcntl F_GETLK/F_SETLK/F_SETLKW).
 * Byte range is [start, end); end == FLOCK_EOF means "to end of file"
 * (l_len == 0). Owner is the posix pid, so locks are not inherited across
 * fork but survive exec, per POSIX. */
#define FLOCK_EOF ((off_t)(((u64)~0ULL) >> 1)) /* max positive off_t */

typedef struct _flock_t {
	struct _flock_t *next, *prev;
	oid_t oid;   /* file identity (port, id) */
	pid_t pid;   /* owning posix pid */
	off_t start; /* inclusive */
	off_t end;   /* exclusive; FLOCK_EOF => to EOF */
	short type;  /* F_RDLCK | F_WRLCK */
} flock_t;


static struct {
	rbtree_t pid;
	lock_t lock;
	id_t fresh;
	char hostname[HOST_NAME_MAX + 1U];
	flock_t *fileLocks;   /* global list of held record locks */
	lock_t fileLocksLock; /* guards fileLocks */
} posix_common;


/* Drop all record locks the process holds on one file (called on any close of
 * an fd referring to the file — POSIX release-on-close semantics). */
static void posix_lockReleaseFile(oid_t oid, pid_t pid);


/* Drop all record locks owned by a process (called on process exit). */
static void posix_lockReleaseProc(pid_t pid);


static process_info_t *_pinfo_find(int pid)
{
	process_info_t pi, *r;

	pi.process = pid;
	r = lib_treeof(process_info_t, linkage, lib_rbFind(&posix_common.pid, &pi.linkage));
	if (r != NULL) {
		r->refs++;
	}

	return r;
}


process_info_t *pinfo_find(int pid)
{
	process_info_t *r;

	(void)proc_lockSet(&posix_common.lock);
	r = _pinfo_find(pid);
	(void)proc_lockClear(&posix_common.lock);
	return r;
}


void pinfo_put(process_info_t *p)
{
	(void)proc_lockSet(&posix_common.lock);
	p->refs--;
	if (p->refs != 0) {
		(void)proc_lockClear(&posix_common.lock);
		return;
	}

	lib_rbRemove(&posix_common.pid, &p->linkage);
	(void)proc_lockClear(&posix_common.lock);

	vm_kfree(p->fds);
	(void)proc_lockDone(&p->lock);
	vm_kfree(p);
}


int posix_fileDeref(open_file_t *f)
{
	int err = EOK;

	(void)proc_lockSet(&f->lock);
	--f->refs;
	if (f->refs == 0) {
		if (f->type == ftConstructing) {
			/* Never got as far as opening anything, so there is nothing to
			 * close -- and its oid names a port that cannot exist, so the
			 * proc_close below would only spend an IPC to be told -EINVAL. */
			err = EOK;
		}
		else if (f->type == ftUnixSocket) {
			err = unix_close((unsigned int)f->oid.id);
		}
		else {
			do {
				err = proc_close(f->oid, f->status);
			} while (err == -EINTR);
		}

		if (f->path != NULL) {
			vm_kfree(f->path);
		}
		(void)proc_lockDone(&f->lock);
		vm_kfree(f);
	}
	else {
		(void)proc_lockClear(&f->lock);
	}
	return err;
}


/* Unrefcounted teardown of a file in an fd slot. Safe ONLY where the process is
 * not reachable by any other thread: its single caller is posix_clone's OOM
 * unwind, which runs before p is inserted into posix_common.pid. Files that ARE
 * published while being built (posix_newFile) must use
 * posix_fileConstructAbort() instead -- see the note there. */
static void posix_putUnusedFile(process_info_t *p, int fd)
{
	open_file_t *f;

	f = p->fds[fd].file;
	(void)proc_lockDone(&f->lock);
	/* Symmetry with posix_fileDeref, which frees both: a free of f that ignores
	 * f->path is the asymmetry that produced the uninitialised-path double free
	 * in the first place. NULL here today, since posix_clone zeroes. */
	if (f->path != NULL) {
		vm_kfree(f->path);
	}
	vm_kfree(f);
	p->fds[fd].file = NULL;
}


/* ---- files under construction -------------------------------------------
 *
 * posix_newFile has to publish p->fds[fd].file before its caller can fill the
 * file in, because the slot is what reserves the descriptor -- and the callers
 * (socket, socketpair, accept4) then run blocking work: unix_accept4 and
 * inet_accept4 wait for a connection to arrive. So the half-built file is
 * reachable by every other thread of the process for an unbounded time.
 *
 * It therefore carries TWO references, one owned by the slot and one by the
 * thread constructing it, exactly as posix_open does. A racing close() can
 * clear the slot and drop its reference without ever reaching zero, so it can
 * neither free the file under the constructor nor leave it orphaned. These two
 * helpers are the only correct ways to end that state.
 */

/* Construction failed. Drop the slot's reference only if the slot still refers
 * to f -- a racing close may already have taken it -- then the construction
 * reference, freeing only if that was the last. */
static void posix_fileConstructAbort(process_info_t *p, int fd, open_file_t *f)
{
	int drop = 1, refs;

	(void)proc_lockSet(&p->lock);

	if (p->fds[fd].file == f) {
		p->fds[fd].file = NULL;
		drop = 2;
	}


	(void)proc_lockSet(&f->lock);
	f->refs -= drop;
	refs = f->refs;
	(void)proc_lockClear(&f->lock);

	if (refs == 0) {
		(void)proc_lockDone(&f->lock);
		if (f->path != NULL) {
			vm_kfree(f->path);
		}
		vm_kfree(f);
	}

	(void)proc_lockClear(&p->lock);
}


/* Construction succeeded. Apply the descriptor flags -- only while the slot is
 * still ours, or we would be setting FD_CLOEXEC on a descriptor a racing close
 * plus a reallocation has handed to someone else -- then release the
 * construction reference. Writing flags unconditionally also clears any value
 * left over from a previous user of the slot, which _posix_allocfd does not
 * reset. */
static void posix_fileConstructDone(process_info_t *p, int fd, open_file_t *f, int cloexec)
{
	(void)proc_lockSet(&p->lock);
	if (p->fds[fd].file == f) {
		p->fds[fd].flags = (cloexec != 0) ? FD_CLOEXEC : 0U;
	}
	(void)proc_lockClear(&p->lock);

	(void)posix_fileDeref(f);
}


int posix_getOpenFile(int fd, open_file_t **f)
{
	process_info_t *p;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -ENOSYS;
	}

	(void)proc_lockSet(&p->lock);
	if ((fd < 0) || (fd >= p->fdsz) || (p->fds[fd].file == NULL)) {
		(void)proc_lockClear(&p->lock);
		pinfo_put(p);
		return -EBADF;
	}

	*f = p->fds[fd].file;

	(void)proc_lockSet(&(*f)->lock);
	(*f)->refs++;
	(void)proc_lockClear(&(*f)->lock);
	(void)proc_lockClear(&p->lock);

	pinfo_put(p);
	return 0;
}


/* Copy the canonical path captured for fd at open time into buf (NUL-terminated).
 * Returns the length (excluding NUL) on success, or -EBADF (no such fd), -ENOENT
 * (fd has no recorded path, e.g. a socket/pipe), -ERANGE (buf too small), -EINVAL.
 * f->path is immutable after open and the ref taken by posix_getOpenFile keeps the
 * open_file_t alive, so the read needs no extra lock. Backs libphoenix fchdir(). */
int posix_fdpath(int fd, char *buf, size_t size)
{
	open_file_t *f;
	size_t len;
	int err;

	if ((buf == NULL) || (size == 0U)) {
		return -EINVAL;
	}

	err = posix_getOpenFile(fd, &f);
	if (err < 0) {
		return err;
	}

	if (f->path == NULL) {
		err = -ENOENT;
	}
	else {
		len = hal_strlen(f->path);
		if (len >= size) {
			err = -ERANGE;
		}
		else {
			hal_memcpy(buf, f->path, len + 1U);
			err = (int)len;
		}
	}

	(void)posix_fileDeref(f);
	return err;
}


static int _posix_allocfd(process_info_t *p, int fd)
{
	fildes_t *nfds;
	int nfdsz = p->fdsz;

	for (; fd < p->maxfd; ++fd) {
		if (fd >= p->fdsz) {
			while (fd >= nfdsz) {
				nfdsz *= 2;
			}

			if (nfdsz > p->maxfd) {
				/* fd can't be >= p->maxfd, so it's always ok */
				nfdsz = p->maxfd;
			}

			nfds = vm_kmalloc((size_t)nfdsz * sizeof(*nfds));
			if (nfds == NULL) {
				return -1;
			}

			hal_memcpy(nfds, p->fds, (size_t)p->fdsz * sizeof(*nfds));
			hal_memset(nfds + p->fdsz, 0, ((size_t)nfdsz - (size_t)p->fdsz) * sizeof(*nfds));

			vm_kfree(p->fds);

			p->fds = nfds;
			p->fdsz = nfdsz;
		}

		if (p->fds[fd].file == NULL) {
			return fd;
		}
	}

	return -1;
}


/* Allocate a descriptor and publish a zeroed open_file_t in its slot, returning
 * the file in *file with a CONSTRUCTION REFERENCE held by the caller. The caller
 * must finish with posix_fileConstructDone() or posix_fileConstructAbort(); see
 * the note above those. */
int posix_newFile(process_info_t *p, int fd, open_file_t **file)
{
	open_file_t *f;

	f = vm_kmalloc(sizeof(open_file_t));
	if (f == NULL) {
		return -ENOMEM;
	}

	(void)proc_lockSet(&p->lock);

	fd = _posix_allocfd(p, fd);
	if (fd < 0) {
		(void)proc_lockClear(&p->lock);
		vm_kfree(f);
		return -EMFILE;
	}

	hal_memset(f, 0, sizeof(open_file_t));
	/* A port that can never be allocated, so a racer that reaches this file
	 * before its owner fills the oid in gets -EINVAL rather than addressing
	 * port 0, which is real and live (port ids come from an idtree seeded at 0). */
	f->oid.port = POSIX_PORT_CONSTRUCTING;
	f->type = ftConstructing;
	f->refs = 2;
	f->offset = 0;

	p->fds[fd].file = f;
	(void)proc_lockInit(&f->lock, &proc_lockAttrDefault, "posix.file");
	(void)proc_lockClear(&p->lock);

	*file = f;
	return fd;
}


int _posix_addOpenFile(process_info_t *p, open_file_t *f, unsigned int flags)
{
	int fd = 0;

	fd = _posix_allocfd(p, fd);
	if (fd < 0) {
		return -EMFILE;
	}

	p->fds[fd].file = f;
	p->fds[fd].flags = flags;

	return fd;
}


static int pinfo_cmp(rbnode_t *n1, rbnode_t *n2)
{
	process_info_t *p1 = lib_treeof(process_info_t, linkage, n1);
	process_info_t *p2 = lib_treeof(process_info_t, linkage, n2);

	/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "Variable pass to lib_treeof will not be NULL, so lib_treeof will not be NULL either" */
	if (p1->process < p2->process) {
		return -1;
	}
	else if (p1->process > p2->process) {
		return 1;
	}
	else {
		return 0;
	}
}


static int posix_truncate(oid_t *oid, off_t length)
{
	msg_t msg;
	int err = -EINVAL;

	if ((oid->port != US_PORT) && (length >= 0)) {
		hal_memset(&msg, 0, sizeof(msg));
		msg.type = mtTruncate;
		hal_memcpy(&msg.oid, oid, sizeof(oid_t));
		msg.i.io.len = (size_t)length;
		err = proc_send(oid->port, &msg);
	}

	return err;
}


int posix_clone(int ppid)
{
	TRACE("clone(%x)", ppid);

	process_info_t *p, *pp;
	process_t *proc;
	int i, j;
	oid_t console;
	open_file_t *f;

	proc = proc_current()->process;

	p = vm_kmalloc(sizeof(process_info_t));
	if (p == NULL) {
		return -ENOMEM;
	}

	hal_memset(&console, 0, sizeof(console));
	(void)proc_lockInit(&p->lock, &proc_lockAttrDefault, "posix.process");
	p->children = NULL;
	p->zombies = NULL;
	p->wait = NULL;
	p->next = p->prev = NULL;
	p->refs = 1;

	pp = pinfo_find(ppid);
	if (pp != NULL) {
		TRACE("clone: got parent");
		(void)proc_lockSet(&pp->lock);
		p->maxfd = pp->maxfd;
		p->fdsz = pp->fdsz;
		p->parent = ppid;
	}
	else {
		p->parent = 0;
		p->maxfd = MAX_FD_COUNT;
		p->fdsz = INITIAL_FD_COUNT;
	}

	p->process = process_getPid(proc);

	p->fds = vm_kmalloc((size_t)p->fdsz * sizeof(fildes_t));
	if (p->fds == NULL) {
		(void)proc_lockDone(&p->lock);
		vm_kfree(p);
		if (pp != NULL) {
			(void)proc_lockClear(&pp->lock);
			pinfo_put(pp);
		}
		return -ENOMEM;
	}

	if (pp != NULL) {
		hal_memcpy(p->fds, pp->fds, (size_t)pp->fdsz * sizeof(fildes_t));

		for (i = 0; i < p->fdsz; ++i) {
			f = p->fds[i].file;
			if (f != NULL) {
				(void)proc_lockSet(&f->lock);
				++f->refs;
				(void)proc_lockClear(&f->lock);
			}
		}

		p->pgid = pp->pgid;
		LIST_ADD(&pp->children, p);
		(void)proc_lockClear(&pp->lock);

		pinfo_put(pp);
	}
	else {
		hal_memset(p->fds, 0, (size_t)p->fdsz * sizeof(fildes_t));

		for (i = 0; i < 3; ++i) {
			f = vm_kmalloc(sizeof(open_file_t));
			p->fds[i].file = f;
			if (f == NULL) {
				for (j = 0; j < i; j++) {
					posix_putUnusedFile(p, j);
				}
				(void)proc_lockDone(&p->lock);
				vm_kfree(p->fds);
				vm_kfree(p);
				return -ENOMEM;
			}

			/* See the note in posix_pipe: zero before use, so the
			 * unconditional vm_kfree(f->path) in posix_fileDeref cannot free
			 * an uninitialised pointer inherited from a recycled block. */
			hal_memset(f, 0, sizeof(open_file_t));

			(void)proc_lockInit(&f->lock, &proc_lockAttrDefault, "posix.file");
			f->refs = 1;
			f->offset = 0;
			f->type = ftTty;
			p->fds[i].flags = 0;
			hal_memcpy(&f->oid, &console, sizeof(oid_t));
		}

		p->fds[0].file->status = O_RDONLY;
		p->fds[1].file->status = O_WRONLY;
		p->fds[2].file->status = O_WRONLY;

		p->pgid = p->process;
	}

	(void)proc_lockSet(&posix_common.lock);
	(void)lib_rbInsert(&posix_common.pid, &p->linkage);
	(void)proc_lockClear(&posix_common.lock);

	return EOK;
}


/* Sweep the fd table, releasing either every descriptor (exit) or just the
 * FD_CLOEXEC ones (exec).
 *
 * The reference is dropped with p->lock RELEASED. posix_fileDeref's last-ref
 * branch sends a blocking proc_close (and allocates a msg_t inside it), so
 * holding the lock across it -- as both sweeps used to -- let exit and exec
 * stall every other thread's open/close/read on this process behind an IPC to a
 * possibly wedged server. Clearing the slot under the lock BEFORE dropping the
 * reference is what makes that safe: whoever clears the slot owns that
 * reference, so no one else can drop it as well.
 *
 * fdsz is snapshotted as an upper bound. The table only ever grows, and p->fds
 * is re-read under the lock on every iteration, so a concurrent
 * _posix_allocfd() reallocating it cannot be followed into freed memory;
 * descriptors created after the snapshot are new and not this sweep's business.
 */
static void posix_sweepFds(process_info_t *p, int cloexecOnly)
{
	int fd, fdsz;

	(void)proc_lockSet(&p->lock);
	fdsz = p->fdsz;
	(void)proc_lockClear(&p->lock);

	for (fd = 0; fd < fdsz; ++fd) {
		open_file_t *f = NULL;

		(void)proc_lockSet(&p->lock);
		if (fd < p->fdsz) {
			f = p->fds[fd].file;
			if ((f != NULL) && ((cloexecOnly == 0) || ((p->fds[fd].flags & FD_CLOEXEC) != 0U))) {
				p->fds[fd].file = NULL;
			}
			else {
				f = NULL;
			}
		}
		(void)proc_lockClear(&p->lock);

		if (f != NULL) {
			(void)posix_fileDeref(f);
		}
	}
}


int posix_exec(void)
{
	TRACE("exec()");

	process_info_t *p;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	posix_sweepFds(p, 1);

	pinfo_put(p);
	return 0;
}


static int posix_exit(process_info_t *p, int code)
{
	p->exitcode = code;

	/* Release every record lock the process held (POSIX release-on-exit). */
	posix_lockReleaseProc(p->process);

	/* Clears each slot before dropping its reference, so a zombie's fd table is
	 * left with no dangling pointers. */
	posix_sweepFds(p, 0);

	return 0;
}


static int posix_create(const char *filename, int type, mode_t mode, oid_t dev, oid_t *oid)
{
	TRACE("posix_create(%s, %d)", filename, mode);

	int err;
	oid_t dir;
	char *name, *basename;
	const char *dirname;

	name = lib_strdup(filename);
	if (name == NULL) {
		return -ENOMEM;
	}

	lib_splitname(name, &basename, &dirname);

	do {
		err = proc_lookup(dirname, NULL, &dir);
		if (err < 0) {
			break;
		}

		err = proc_create(dir.port, type, mode, dev, dir, basename, oid);
		if (err < 0) {
			break;
		}

		err = EOK;
	} while (0);

	vm_kfree(name);
	return err;
}

int posix_statvfs(const char *path, int fildes, struct statvfs *buf)
{
	oid_t oid, dev;
	oid_t *oidp, *devp;
	open_file_t *f;
	msg_t msg;
	int err = EOK;

	if (((path == NULL) && (fildes < 0)) ||
			((path != NULL) && (fildes != -1))) {
		return -EINVAL;
	}

	if (path == NULL) {
		err = posix_getOpenFile(fildes, &f);
		if (err < 0) {
			return err;
		}
		oidp = &f->oid;
		devp = NULL;
	}
	else {
		if (proc_lookup(path, &oid, &dev) < 0) {
			return -ENOENT;
		}
		oidp = &oid;
		devp = &dev;
	}

	/* Detect mountpoint */
	if ((devp != NULL) && (oidp->port != devp->port)) {
		hal_memset(&msg, 0, sizeof(msg));
		msg.type = mtGetAttr;
		hal_memcpy(&msg.oid, oidp, sizeof(*oidp));
		msg.i.attr.type = atMode;

		if ((proc_send(oidp->port, &msg) < 0) || (msg.o.err < 0)) {
			return -EIO;
		}

		if (S_ISDIR((unsigned long long)msg.o.attr.val)) {
			oidp = devp;
		}
	}

	hal_memset(buf, 0, sizeof(*buf));

	hal_memset(&msg, 0, sizeof(msg));
	msg.type = mtStat;
	msg.o.data = buf;
	msg.o.size = sizeof(*buf);

	if (proc_send(oidp->port, &msg) < 0) {
		err = -EIO;
	}
	else {
		err = msg.o.err;
	}

	if (path == NULL) {
		if (err == EOK) {
			err = posix_fileDeref(f);
		}
		else {
			(void)posix_fileDeref(f);
		}
	}

	return err;
}


/* TODO: handle O_CREAT and O_EXCL */
int posix_open(const char *filename, int oflag, u8 *ustack)
{
	TRACE("open(%s, %d, %d)", filename, oflag);
	oid_t ln, oid, dev, pipesrv;
	int fd = 0, err = 0, created = 0;
	int drop, refs, ours;
	process_info_t *p;
	open_file_t *f;
	mode_t mode;
	off_t size;

	if (proc_lookup("/dev/posix/pipes", NULL, &pipesrv) < 0) {
		hal_memset(&pipesrv, 0xff, sizeof(oid_t));
	}

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	hal_memset(&dev, 0, sizeof(oid_t));

	(void)proc_lockSet(&p->lock);

	do {
		fd = _posix_allocfd(p, fd);
		if (fd < 0) {
			err = -EMFILE;
			break;
		}

		f = vm_kmalloc(sizeof(open_file_t));
		if (f == NULL) {
			err = -ENOMEM;
			break;
		}

		/* The fd slot is how the descriptor is RESERVED across the blocking
		 * IPCs below (_posix_allocfd looks for file == NULL), so f has to be
		 * published before it is filled in. That makes it reachable by every
		 * other thread of this process while its fields are still garbage, so:
		 *
		 *  - zero it, giving deterministic fields instead of whatever the
		 *    recycled block held, and point oid at a port that cannot exist:
		 *    a zeroed oid is NOT harmless, because port ids come from an
		 *    idtree that starts at 0, so port 0 is a real, live port and a
		 *    racer's read/write would address it. POSIX_PORT_CONSTRUCTING can
		 *    never be allocated, so proc_portGet() fails to find it and such a
		 *    call returns -EINVAL; and
		 *  - take TWO references: one owned by the fd slot, one owned by this
		 *    thread for the duration of the construction.
		 *
		 * The construction reference is what makes a racing close() safe. It
		 * clears the slot and drops the slot's reference, but cannot reach zero
		 * and so cannot free f while we are still writing into it; our own drop
		 * at the end of the open then frees it. Without it, a close (or an
		 * exit-time sweep over all fds) between here and the tail below
		 * decremented an uninitialised refs and could free f under us. */
		hal_memset(f, 0, sizeof(open_file_t));
		f->oid.port = POSIX_PORT_CONSTRUCTING;
		f->type = ftConstructing;
		f->refs = 2;

		p->fds[fd].file = f;
		(void)proc_lockInit(&f->lock, &proc_lockAttrDefault, "posix.file");
		(void)proc_lockClear(&p->lock);

		do {
			err = proc_lookup(filename, &ln, &oid);

			if ((err == -ENOENT) && (((unsigned int)oflag & O_CREAT) != 0U)) {
				GETFROMSTACK(ustack, mode_t, mode, 2U);

				err = posix_create(filename, 1 /* otFile */, mode | S_IFREG, dev, &oid);
				if (err < 0) {
					break;
				}
				created = 1;
				hal_memcpy(&ln, &oid, sizeof(oid_t));
			}
			else if (err < 0) {
				break;
			}
			else {
				/* No action required */
			}

			if (oid.port != US_PORT) {
				err = proc_open(oid, (unsigned int)oflag);
				if (err < 0) {
					break;
				}
			}

			(void)proc_lockSet(&p->lock);
			/* Only if this descriptor is still ours. A concurrent close()
			 * clears the slot, and another thread's open() can then reallocate
			 * the same fd number (_posix_allocfd looks for file == NULL) -- an
			 * unguarded write would set or clear FD_CLOEXEC on that thread's
			 * descriptor instead of ours. */
			if (p->fds[fd].file == f) {
				p->fds[fd].flags = ((unsigned int)oflag & O_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
			}
			(void)proc_lockClear(&p->lock);

			if (err == 0) {
				hal_memcpy(&f->oid, &oid, sizeof(oid));
			}
			else {
				/* multiplexer, e.g. /dev/ptmx */
				f->oid.port = oid.port;
				f->oid.id = (unsigned int)err;
			}

			hal_memcpy(&f->ln, &ln, sizeof(ln));

			/* TODO: check for other types */
			if (oid.port == US_PORT) {
				f->type = ftUnixSocket;
			}
			else if (oid.port == pipesrv.port && proc_size(f->oid) < 0) {
				/* FIXME: replace this hacky solution with proper device driver recognition */
				f->type = ftPipe;
			}
			else {
				f->type = ftRegular;
			}

			f->offset = 0;

			if (((unsigned int)oflag & O_TRUNC) != 0U) {
				(void)posix_truncate(&f->oid, 0);
			}
			else if (((unsigned int)oflag & O_APPEND) != 0U) {
				/* Keep offset at 0 for files that cannot report their size (e.g. devices) */
				size = proc_size(f->oid);
				if (size > 0) {
					f->offset = size;
				}
			}
			else {
				/* No action required */
			}

			f->status = (unsigned int)oflag & ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_CLOEXEC);

			/* Record the canonical path (libphoenix resolves it before sys_open) so
			 * fchdir()/the *at family can recover a fd's directory. Shared across
			 * dup() via the refcounted open_file_t. Best-effort: on OOM leave NULL
			 * (fchdir then fails cleanly rather than acting on a stale cwd). */
			{
				size_t plen = hal_strlen(filename);
				char *path = vm_kmalloc(plen + 1U);
				if (path != NULL) {
					hal_memcpy(path, filename, plen + 1U);
					/* Publish the pointer only once the buffer holds the string.
					 * f is already reachable by other threads of this process,
					 * so a posix_fdpath() that read f->path between the
					 * allocation and the copy would hal_strlen() uninitialised
					 * memory and run past the end of the block. */
					f->path = path;
				}
			}

			/* Did we keep the descriptor? A concurrent close() clears the
			 * slot, and _posix_allocfd can then hand that number to a
			 * DIFFERENT file, so returning it would point the caller at
			 * someone else's open file. */
			(void)proc_lockSet(&p->lock);
			ours = (p->fds[fd].file == f) ? 1 : 0;
			(void)proc_lockClear(&p->lock);

			/* Release the construction reference. Normally this just takes
			 * refs 2 -> 1, leaving the fd slot as the only owner. If a
			 * concurrent close() already cleared the slot, this is the last
			 * reference and f is closed and freed here -- nothing leaks and
			 * nothing is used after being freed. */
			(void)posix_fileDeref(f);

			pinfo_put(p);
			return (ours != 0) ? fd : -EBADF;
		} while (0);

		if (created != 0) {
			/* file was created in the filesystem - we should unlink it now */
			(void)posix_unlink(filename);
			(void)proc_destroy(oid.port, oid);
		}

		(void)proc_lockSet(&p->lock);

		/* Drop the construction reference, plus the fd slot's reference if the
		 * slot still refers to f -- a concurrent close() may already have taken
		 * that one. Freeing unconditionally here (as this used to) would pull
		 * the file out from under such a racer. p->lock -> f->lock is the order
		 * posix_getOpenFile establishes. */
		drop = 1;
		if (p->fds[fd].file == f) {
			p->fds[fd].file = NULL;
			drop = 2;
		}

		(void)proc_lockSet(&f->lock);
		f->refs -= drop;
		refs = f->refs;
		(void)proc_lockClear(&f->lock);

		if (refs == 0) {
			(void)proc_lockDone(&f->lock);
			if (f->path != NULL) {
				vm_kfree(f->path);
			}
			vm_kfree(f);
		}

	} while (0);

	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


int posix_close(int fildes)
{
	TRACE("close(%d)", fildes);
	open_file_t *f;
	process_info_t *p;
	int err = -EBADF;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	(void)proc_lockSet(&p->lock);

	do {
		if ((fildes < 0) || (fildes >= p->fdsz)) {
			break;
		}

		if (p->fds[fildes].file == NULL) {
			break;
		}

		f = p->fds[fildes].file;
		p->fds[fildes].file = NULL;
		(void)proc_lockClear(&p->lock);

		/* POSIX: closing any fd on a file drops all of this process's record
		 * locks on that file. Only regular files can hold record locks. */
		if (f->type == ftRegular) {
			posix_lockReleaseFile(f->oid, process_getPid(proc_current()->process));
		}

		pinfo_put(p);
		return posix_fileDeref(f);
	} while (0);

	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


ssize_t posix_read(int fildes, void *buf, size_t nbyte, off_t offset)
{
	TRACE("read(%d, %p, %zu, %jd)", fildes, buf, nbyte, (intmax_t)offset);

	open_file_t *f;
	ssize_t rcnt;
	off_t offs = offset;
	unsigned int status;
	int err;

	err = posix_getOpenFile(fildes, &f);
	if (err < 0) {
		return err;
	}

	if ((f->status & O_WRONLY) != 0U) {
		(void)posix_fileDeref(f);
		return -EBADF;
	}

	if (offset >= 0 && !F_SEEKABLE(f->type)) {
		(void)posix_fileDeref(f);
		return -ESPIPE;
	}

	(void)proc_lockSet(&f->lock);
	/* offset < 0 means use current fd offset */
	if (offset < 0) {
		offs = f->offset;
	}
	status = f->status;
	(void)proc_lockClear(&f->lock);

	if (f->type == ftUnixSocket) {
		rcnt = unix_recvfrom((unsigned int)f->oid.id, buf, nbyte, 0, NULL, NULL);
	}
	else {
		rcnt = proc_read(f->oid, offs, buf, nbyte, status);
	}

	if ((rcnt > 0) && ((size_t)rcnt > nbyte)) {
		rcnt = (ssize_t)nbyte;
	}

	if (rcnt > 0 && offset < 0 && F_SEEKABLE(f->type)) {
		(void)proc_lockSet(&f->lock);
		f->offset += rcnt;
		(void)proc_lockClear(&f->lock);
	}

	(void)posix_fileDeref(f);

	return rcnt;
}


static void posix_sigpipe(void)
{
	thread_t *curr = proc_current();

	(void)threads_sigpost(curr->process, curr, SIGPIPE);
}


ssize_t posix_write(int fildes, void *buf, size_t nbyte, off_t offset)
{
	TRACE("write(%d, %p, %zu, %jd)", fildes, buf, nbyte, (intmax_t)offset);

	open_file_t *f;
	ssize_t rcnt;
	off_t offs = offset;
	unsigned int status;
	int err;

	err = posix_getOpenFile(fildes, &f);
	if (err < 0) {
		return err;
	}

	if ((f->status & O_RDONLY) != 0U) {
		(void)posix_fileDeref(f);
		return -EBADF;
	}

	if (offset >= 0 && !F_SEEKABLE(f->type)) {
		(void)posix_fileDeref(f);
		return -ESPIPE;
	}

	(void)proc_lockSet(&f->lock);
	/* offset < 0 means use current fd offset */
	if (offset < 0) {
		offs = f->offset;
	}
	status = f->status;
	(void)proc_lockClear(&f->lock);

	if (f->type == ftUnixSocket) {
		rcnt = unix_sendto((unsigned int)f->oid.id, buf, nbyte, 0, NULL, 0);
	}
	else {
		rcnt = proc_write(f->oid, offs, buf, nbyte, status);

		if (rcnt > 0 && offset < 0 && F_SEEKABLE(f->type)) {
			(void)proc_lockSet(&f->lock);
			f->offset += rcnt;
			(void)proc_lockClear(&f->lock);
		}
	}

	if ((rcnt == -EPIPE) && ((f->type == ftUnixSocket) || (f->type == ftPipe) || (f->type == ftFifo) || (f->type == ftInetSocket))) {
		/* NOTE: for a UNIX socket, SIGPIPE shall be sent if the socket has been shut down
		 * for writing or is no longer connected. The latter case applies only to SOCK_STREAM
		 * sockets. Currently, shutdown() closes the socket altogether, so unix_sendto()
		 * cannot return EPIPE for a socket shut down for writing. unix_sendto() must also
		 * not return EPIPE for no longer connected SOCK_DGRAM sockets, because SIGPIPE is not
		 * required for them.
		 */
		posix_sigpipe();
	}

	(void)posix_fileDeref(f);

	return rcnt;
}


int posix_getOid(int fildes, oid_t *oid)
{
	open_file_t *f;
	int err;

	err = posix_getOpenFile(fildes, &f);
	if (err < 0) {
		return err;
	}

	hal_memcpy(oid, &f->oid, sizeof(oid_t));

	(void)posix_fileDeref(f);

	return EOK;
}


int posix_dup(int fildes)
{
	TRACE("dup(%d)", fildes);

	process_info_t *p;
	int newfd = 0;
	open_file_t *f;
	int err;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	(void)proc_lockSet(&p->lock);

	do {
		if ((fildes < 0) || (fildes >= p->fdsz)) {
			err = -EBADF;
			break;
		}

		if (p->fds[fildes].file == NULL) {
			err = -EBADF;
			break;
		}

		f = p->fds[fildes].file;
		newfd = _posix_allocfd(p, newfd);
		if (newfd < 0) {
			err = -EMFILE;
			break;
		}

		p->fds[newfd].file = f;
		p->fds[newfd].flags = 0;
		(void)proc_lockSet(&f->lock);
		f->refs++;
		(void)proc_lockClear(&f->lock);
		(void)proc_lockClear(&p->lock);
		pinfo_put(p);

		return newfd;
	} while (0);

	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


static int _posix_dup2(process_info_t *p, int fildes, int fildes2)
{
	open_file_t *f, *f2;
	int nfd2;

	if ((fildes < 0) || (fildes >= p->fdsz)) {
		return -EBADF;
	}

	if ((fildes2 < 0) || (fildes2 >= p->maxfd)) {
		return -EBADF;
	}

	if (p->fds[fildes].file == NULL) {
		return -EBADF;
	}

	if (fildes == fildes2) {
		return fildes2;
	}

	if (fildes2 >= p->fdsz) {
		/* requested fd bigger than current table, resize to match */
		nfd2 = _posix_allocfd(p, fildes2);

		/* sanity check */
		if (nfd2 != fildes2) {
			return -EFAULT;
		}
	}

	f = p->fds[fildes].file;
	f2 = p->fds[fildes2].file;

	if (p->fds[fildes2].file != NULL) {
		p->fds[fildes2].file = NULL;
		(void)posix_fileDeref(f2);
	}

	p->fds[fildes2].file = f;
	p->fds[fildes2].flags = 0;

	(void)proc_lockSet(&f->lock);
	f->refs++;
	(void)proc_lockClear(&f->lock);

	return fildes2;
}


int posix_dup2(int fildes, int fildes2)
{
	TRACE("dup2(%d, %d)", fildes, fildes2);

	process_info_t *p;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	(void)proc_lockSet(&p->lock);
	fildes2 = _posix_dup2(p, fildes, fildes2);
	(void)proc_lockClear(&p->lock);
	pinfo_put(p);

	return fildes2;
}


int posix_pipe(int fildes[2])
{
	TRACE("pipe(%p)", fildes);

	process_info_t *p;
	open_file_t *fi, *fo;
	oid_t oid;
	oid_t pipesrv;
	int res;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	hal_memset(&oid, 0, sizeof(oid));

	res = proc_lookup("/dev/posix/pipes", NULL, &pipesrv);
	if (res < 0) {
		pinfo_put(p);
		return (res == -EINTR) ? res : -ENOSYS;
	}

	res = proc_create(pipesrv.port, pxBufferedPipe, O_RDONLY | O_WRONLY, oid, pipesrv, NULL, &oid);
	if (res < 0) {
		pinfo_put(p);
		return res;
	}

	fo = vm_kmalloc(sizeof(open_file_t));
	if (fo == NULL) {
		(void)proc_destroy(oid.port, oid);
		pinfo_put(p);
		return -ENOMEM;
	}

	fi = vm_kmalloc(sizeof(open_file_t));
	if (fi == NULL) {
		vm_kfree(fo);
		(void)proc_destroy(oid.port, oid);
		pinfo_put(p);
		return -ENOMEM;
	}

	/* vm_kmalloc does not zero, and posix_fileDeref frees f->path
	 * unconditionally when it is non-NULL. A pipe never sets a path, so
	 * without this the close of a pipe fd frees whatever the recycled block
	 * happens to hold in that slot -- and a block last used by a regular file
	 * holds a real (already freed) path pointer, which is in range and
	 * block-aligned, so both _vm_zfree guards pass and the block lands on the
	 * free list twice. posix_newFile already zeroes for this reason; do the
	 * same here rather than adding two more field assignments, so a field
	 * added to open_file_t later cannot reintroduce this. Also covers f->ln,
	 * which was likewise left uninitialised (fstat on a pipe read it). */
	hal_memset(fo, 0, sizeof(open_file_t));
	hal_memset(fi, 0, sizeof(open_file_t));

	(void)proc_lockSet(&p->lock);
	fildes[0] = _posix_allocfd(p, 0);
	if (fildes[0] >= 0) {
		fildes[1] = _posix_allocfd(p, fildes[0] + 1);
	}

	if ((fildes[0] < 0) || (fildes[1] < 0)) {
		(void)proc_lockClear(&p->lock);

		vm_kfree(fo);
		vm_kfree(fi);

		(void)proc_destroy(oid.port, oid);

		pinfo_put(p);
		return -EMFILE;
	}

	p->fds[fildes[0]].flags = p->fds[fildes[1]].flags = 0;

	p->fds[fildes[0]].file = fo;
	(void)proc_lockInit(&fo->lock, &proc_lockAttrDefault, "posix.file");
	hal_memcpy(&fo->oid, &oid, sizeof(oid));
	fo->refs = 1;
	fo->offset = 0;
	fo->type = ftPipe;
	fo->status = O_RDONLY;

	p->fds[fildes[1]].file = fi;
	(void)proc_lockInit(&fi->lock, &proc_lockAttrDefault, "posix.file");
	hal_memcpy(&fi->oid, &oid, sizeof(oid));
	fi->refs = 1;
	fi->offset = 0;
	fi->type = ftPipe;
	fi->status = O_WRONLY;

	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return 0;
}


int posix_mkfifo(const char *pathname, mode_t mode)
{
	TRACE("mkfifo(%s, %x)", pathname, mode);

	oid_t oid, file;
	oid_t pipesrv;
	int ret;

	hal_memset(&oid, 0, sizeof(oid));

	if (proc_lookup("/dev/posix/pipes", NULL, &pipesrv) < 0) {
		return -ENOSYS;
	}

	ret = proc_create(pipesrv.port, pxBufferedPipe, 0U, oid, pipesrv, NULL, &oid);
	if (ret < 0) {
		return ret;
	}

	/* link pipe in posix server */
	ret = proc_link(oid, oid, pathname);
	if (ret < 0) {
		(void)proc_destroy(oid.port, oid);
		return ret;
	}

	/* create pipe in filesystem */
	ret = posix_create(pathname, 2 /* otDev */, mode | S_IFIFO, oid, &file);
	if (ret < 0) {
		(void)proc_unlink(oid, oid, pathname);
		(void)proc_destroy(oid.port, oid);
		return ret;
	}

	return 0;
}


int posix_chmod(const char *pathname, mode_t mode)
{
	TRACE("chmod(%s, %x)", pathname, mode);

	oid_t oid;
	msg_t msg;
	int err;

	if (proc_lookup(pathname, &oid, NULL) < 0) {
		return -ENOENT;
	}

	hal_memset(&msg, 0, sizeof(msg));
	hal_memcpy(&msg.oid, &oid, sizeof(oid));

	msg.type = mtSetAttr;
	msg.i.attr.type = atMode;
	/* parasoft-suppress-next-line MISRAC2012-RULE_10_3-b */
	msg.i.attr.val = mode & ALLPERMS;

	err = proc_send(oid.port, &msg);
	if (err >= 0) {
		err = msg.o.err;
	}

	return (err < 0) ? err : EOK;
}


int posix_link(const char *path1, const char *path2)
{
	TRACE("link(%s, %s)", path1, path2);

	oid_t oid, dev, dir;
	int err;
	char *name, *basename;
	const char *dirname;

	name = lib_strdup(path2);
	if (name == NULL) {
		return -ENOMEM;
	}

	(void)lib_splitname(name, &basename, &dirname);

	do {
		err = proc_lookup(dirname, NULL, &dir);
		if (err < 0) {
			break;
		}

		err = proc_lookup(path1, &oid, &dev);
		if (err < 0) {
			break;
		}

		if (oid.port != dir.port) {
			err = -EXDEV;
			break;
		}

		err = proc_link(dir, oid, basename);
		if (err < 0) {
			break;
		}
		if (dev.port != oid.port) {
			/* Signal link to device */
			/* FIXME: refcount here? */
			err = proc_link(dev, dev, path2);
			if (err < 0) {
				break;
			}
		}

		err = EOK;
	} while (0);

	vm_kfree(name);
	return err;
}


int posix_unlink(const char *pathname)
{
	TRACE("unlink(%s)", pathname);

	oid_t oid, dir;
	int err;
	char *name, *basename;
	const char *dirname;

	name = lib_strdup(pathname);
	if (name == NULL) {
		return -ENOMEM;
	}

	(void)lib_splitname(name, &basename, &dirname);

	do {
		err = proc_lookup(dirname, NULL, &dir);
		if (err < 0) {
			break;
		}

		err = proc_lookup(pathname, NULL, &oid);
		if (err < 0) {
			break;
		}

		err = proc_unlink(dir, oid, basename);
		if (err < 0) {
			break;
		}

		if (dir.port != oid.port) {
			if (oid.port == US_PORT) {
				(void)unix_unlink((unsigned int)oid.id);
			}
			else {
				/* Signal unlink to device */
				/* FIXME: refcount here? */
				err = proc_unlink(oid, oid, pathname);
				if (err < 0) {
					break;
				}
			}
		}

		err = EOK;
	} while (0);

	vm_kfree(name);
	return err;
}


int posix_lseek(int fildes, off_t *offset, int whence)
{
	TRACE("seek(%d, %d, %d)", fildes, offset, whence);

	open_file_t *f;
	off_t scnt;
	int err = 0;

	err = posix_getOpenFile(fildes, &f);
	if (err != 0) {
		return err;
	}

	if (!F_SEEKABLE(f->type)) {
		(void)posix_fileDeref(f);
		return -ESPIPE;
	}

	(void)proc_lockSet(&f->lock);
	switch (whence) {
		case SEEK_SET:
			scnt = *offset;
			break;

		case SEEK_CUR:
			scnt = f->offset + *offset;
			break;

		case SEEK_END:
			scnt = proc_size(f->oid);
			if (scnt < 0) {
				err = (int)scnt;
				break;
			}
			scnt += *offset;
			break;

		default:
			scnt = -1;
			break;
	}

	if (scnt >= 0) {
		f->offset = scnt;
	}
	else if (err == 0) {
		err = -EINVAL;
	}
	else {
		/* No action required */
	}

	(void)proc_lockClear(&f->lock);

	(void)posix_fileDeref(f);

	if (err == 0) {
		*offset = scnt;
	}

	return err;
}


int posix_ftruncate(int fildes, off_t length)
{
	TRACE("ftruncate(%d)", fildes);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(fildes, &f);
	if (err >= 0) {
		if ((f->status & O_RDONLY) == 0U) {
			err = posix_truncate(&f->oid, length);
		}
		else {
			err = -EBADF;
		}
		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_fstat(int fd, struct stat *buf)
{
	TRACE("fstat(%d)", fd);

	open_file_t *f;
	msg_t msg;
	int err;
	struct _attrAll attrs;

	err = posix_getOpenFile(fd, &f);
	if (err < 0) {
		return err;
	}

	if (f->type == ftConstructing) {
		/* A racing fstat() on a descriptor whose open() has not returned yet.
		 * Nothing can be reported about it, and the mostly-zeroed stat that the
		 * socket/pipe path below would hand back -- correct for those, since
		 * there is nothing to query -- would be silently wrong data here. */
		(void)posix_fileDeref(f);
		return -EBADF;
	}

	hal_memset(buf, 0, sizeof(struct stat));
	hal_memset(&msg, 0, sizeof(msg_t));

	buf->st_dev = (dev_t)f->ln.port;
	buf->st_ino = (ino_t)f->ln.id;
	buf->st_rdev = (dev_t)f->oid.port;

	if (f->type == ftRegular) {
		msg.type = mtGetAttrAll;
		hal_memcpy(&msg.oid, &f->oid, sizeof(oid_t));
		msg.o.data = &attrs;
		msg.o.size = sizeof(attrs);

		do {
			err = proc_send(f->oid.port, &msg);
			if (err < 0) {
				break;
			}

			err = msg.o.err;
			if (err < 0) {
				break;
			}

			err = attrs.mTime.err;
			if (err < 0) {
				break;
			}
			buf->st_mtim.tv_sec = (time_t)attrs.mTime.val;
			buf->st_mtim.tv_nsec = 0;

			err = attrs.aTime.err;
			if (err < 0) {
				break;
			}

			buf->st_atim.tv_sec = (time_t)attrs.aTime.val;
			buf->st_atim.tv_nsec = 0;

			err = attrs.cTime.err;
			if (err < 0) {
				break;
			}
			buf->st_ctim.tv_sec = (time_t)attrs.cTime.val;
			buf->st_ctim.tv_nsec = 0;

			err = attrs.links.err;
			if (err < 0) {
				break;
			}
			buf->st_nlink = (nlink_t)attrs.links.val;

			err = attrs.mode.err;
			if (err < 0) {
				break;
			}
			buf->st_mode = (mode_t)attrs.mode.val;

			err = attrs.uid.err;
			if (err < 0) {
				break;
			}
			buf->st_uid = (uid_t)attrs.uid.val;

			err = attrs.gid.err;
			if (err < 0) {
				break;
			}
			buf->st_gid = (gid_t)attrs.gid.val;

			err = attrs.size.err;
			if (err < 0) {
				break;
			}
			buf->st_size = (off_t)attrs.size.val;

			err = attrs.blocks.err;
			if (err < 0) {
				break;
			}
			buf->st_blocks = (blkcnt_t)attrs.blocks.val;

			err = attrs.ioblock.err;
			if (err < 0) {
				break;
			}
			buf->st_blksize = (blksize_t)attrs.ioblock.val;
		} while (0);
	}
	else {
		switch (f->type) {
			case ftRegular:
				break;
			case ftPipe:
			case ftFifo:
				buf->st_mode = S_IFIFO;
				break;
			case ftInetSocket:
			case ftUnixSocket:
				buf->st_mode = S_IFSOCK;
				break;
			case ftTty:
				buf->st_mode = S_IFCHR;
				break;
			default:
				buf->st_mode = 0;
				break;
		}

		buf->st_uid = 0;
		buf->st_gid = 0;
		buf->st_size = proc_size(f->oid);
	}

	(void)posix_fileDeref(f);

	return err;
}


int posix_fsync(int fd)
{
	TRACE("fsync(%d)", fd);

	open_file_t *f;
	msg_t msg;
	int err;

	err = posix_getOpenFile(fd, &f);
	if (err < 0) {
		return err;
	}

	hal_memset(&msg, 0, sizeof(msg_t));

	/* FIXME: Replace this hack, pass oid via msg_t root struct */
	msg.type = 0xf52; /* mtSync */

	hal_memcpy(msg.i.raw, &f->oid, sizeof(f->oid));

	err = proc_send(f->oid.port, &msg);

	(void)posix_fileDeref(f);

	return err;
}


static int posix_fcntlDup(int fd, int fd2, int cloexec)
{
	process_info_t *p;
	int err;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	(void)proc_lockSet(&p->lock);
	if ((fd < 0) || (fd >= p->fdsz) || (fd2 < 0) || (fd2 >= p->maxfd)) {
		(void)proc_lockClear(&p->lock);
		pinfo_put(p);
		return -EBADF;
	}

	fd2 = _posix_allocfd(p, fd2);
	/* parasoft-suppress-next-line MISRAC2012-DIR_4_7 "Returned value checked in function (_posix_dup2) */
	err = _posix_dup2(p, fd, fd2);
	if ((err == fd2) && (cloexec != 0)) {
		p->fds[fd2].flags = FD_CLOEXEC;
	}

	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


static int posix_fcntlSetFd(int fd, unsigned int flags)
{
	process_info_t *p;
	int err = EOK;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -ENOSYS;
	}

	(void)proc_lockSet(&p->lock);
	if ((fd < 0) || (fd >= p->fdsz)) {
		(void)proc_lockClear(&p->lock);
		pinfo_put(p);
		return -EBADF;
	}

	if (p->fds[fd].file != NULL) {
		p->fds[fd].flags = flags;
	}
	else {
		err = -EBADF;
	}
	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


static int posix_fcntlGetFd(int fd)
{
	process_info_t *p;
	int err;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -ENOSYS;
	}

	(void)proc_lockSet(&p->lock);
	if ((fd < 0) || (fd >= p->fdsz)) {
		(void)proc_lockClear(&p->lock);
		pinfo_put(p);
		return -EBADF;
	}

	if (p->fds[fd].file != NULL) {
		err = (int)p->fds[fd].flags;
	}
	else {
		err = -EBADF;
	}
	(void)proc_lockClear(&p->lock);
	pinfo_put(p);
	return err;
}


static int posix_fcntlSetFl(int fd, unsigned int val)
{
	open_file_t *f;
	int err;
	/* Creation and access mode flags shall be ignored */
	unsigned int ignorefl = O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_RDONLY | O_RDWR | O_WRONLY;

	err = posix_getOpenFile(fd, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_setfl(f->oid.port, val);
				break;
			case ftUnixSocket:
				err = unix_setfl((unsigned int)f->oid.id, val);
				break;
			default:
				f->status = (val & ~ignorefl) | (f->status & ignorefl);
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


static int posix_fcntlGetFl(int fd)
{
	open_file_t *f;
	int err;

	err = posix_getOpenFile(fd, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_getfl(f->oid.port);
				break;
			case ftUnixSocket:
				err = unix_getfl((unsigned int)f->oid.id);
				break;
			default:
				err = (int)f->status;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


static int flock_overlap(off_t s1, off_t e1, off_t s2, off_t e2)
{
	return (s1 < e2) && (s2 < e1);
}


static int flock_oidSame(const oid_t *a, const oid_t *b)
{
	return (a->port == b->port) && (a->id == b->id);
}


/* Find the first held lock that would conflict with a [start,end) lock of the
 * given type requested by `pid` on `oid`. A conflict is an overlap owned by a
 * different pid where at least one side is a write lock. Caller holds
 * fileLocksLock. */
static flock_t *_posix_lockConflict(oid_t oid, pid_t pid, off_t start, off_t end, short type)
{
	flock_t *it = posix_common.fileLocks;

	if (it == NULL) {
		return NULL;
	}

	do {
		if (flock_oidSame(&it->oid, &oid) && (it->pid != pid) &&
				flock_overlap(start, end, it->start, it->end) &&
				((type == F_WRLCK) || (it->type == F_WRLCK))) {
			return it;
		}
		it = it->next;
	} while (it != posix_common.fileLocks);

	return NULL;
}


/* Remove/split all locks owned by (pid,oid) within [start,end). At most one
 * held lock can straddle both ends (locks of one owner never overlap), so at
 * most one split — for which `spare` is consumed; *spareUsed reports whether.
 * Caller holds fileLocksLock. */
static void _posix_lockClearRange(oid_t oid, pid_t pid, off_t start, off_t end, flock_t *spare, int *spareUsed)
{
	flock_t *l, *it;
	int leftKeep, rightKeep;

	*spareUsed = 0;

	for (;;) {
		l = NULL;
		it = posix_common.fileLocks;
		if (it != NULL) {
			do {
				if (flock_oidSame(&it->oid, &oid) && (it->pid == pid) &&
						flock_overlap(start, end, it->start, it->end)) {
					l = it;
					break;
				}
				it = it->next;
			} while (it != posix_common.fileLocks);
		}

		if (l == NULL) {
			break;
		}

		leftKeep = (l->start < start) ? 1 : 0;
		rightKeep = (l->end > end) ? 1 : 0;

		if ((leftKeep != 0) && (rightKeep != 0)) {
			/* [start,end) is fully inside l: shrink l to the left piece and
			 * add the right piece. Nothing else can overlap [start,end). */
			spare->oid = oid;
			spare->pid = pid;
			spare->type = l->type;
			spare->start = end;
			spare->end = l->end;
			l->end = start;
			LIST_ADD(&posix_common.fileLocks, spare);
			*spareUsed = 1;
			break;
		}
		else if (leftKeep != 0) {
			l->end = start;
		}
		else if (rightKeep != 0) {
			l->start = end;
		}
		else {
			LIST_REMOVE(&posix_common.fileLocks, l);
			vm_kfree(l);
		}
	}
}


static void posix_lockReleaseFile(oid_t oid, pid_t pid)
{
	int spareUsed;

	(void)proc_lockSet(&posix_common.fileLocksLock);
	/* full range: end == FLOCK_EOF so no lock can straddle the right edge and
	 * no split is possible -> spare unused. */
	_posix_lockClearRange(oid, pid, 0, FLOCK_EOF, NULL, &spareUsed);
	(void)proc_lockClear(&posix_common.fileLocksLock);
}


static void posix_lockReleaseProc(pid_t pid)
{
	flock_t *it, *next;

	(void)proc_lockSet(&posix_common.fileLocksLock);
	it = posix_common.fileLocks;
	while (it != NULL) {
		next = it->next;
		if (it->pid == pid) {
			LIST_REMOVE(&posix_common.fileLocks, it);
			vm_kfree(it);
			/* list head may have moved / emptied; restart the scan */
			it = posix_common.fileLocks;
			continue;
		}
		if (next == posix_common.fileLocks) {
			break;
		}
		it = next;
	}
	(void)proc_lockClear(&posix_common.fileLocksLock);
}


static int posix_fcntlLock(int fd, unsigned int cmd, struct flock *ulock)
{
	open_file_t *f;
	struct flock lock;
	oid_t oid;
	pid_t pid;
	off_t start, end, fsize;
	flock_t *spare1, *spare2, *conflict;
	short type;
	int err, spareUsed;

	if (vm_mapBelongs(proc_current()->process, ulock, sizeof(*ulock)) < 0) {
		return -EFAULT;
	}
	hal_memcpy(&lock, ulock, sizeof(lock));

	type = lock.l_type;
	if ((type != F_RDLCK) && (type != F_WRLCK) && (type != F_UNLCK)) {
		return -EINVAL;
	}
	if ((cmd == (unsigned int)F_GETLK) && (type == F_UNLCK)) {
		return -EINVAL;
	}

	err = posix_getOpenFile(fd, &f);
	if (err < 0) {
		return err;
	}

	if (!F_SEEKABLE(f->type)) {
		(void)posix_fileDeref(f);
		return -EINVAL;
	}

	switch (lock.l_whence) {
		case SEEK_SET:
			start = 0;
			break;

		case SEEK_CUR:
			(void)proc_lockSet(&f->lock);
			start = f->offset;
			(void)proc_lockClear(&f->lock);
			break;

		case SEEK_END:
			fsize = proc_size(f->oid);
			if (fsize < 0) {
				(void)posix_fileDeref(f);
				return (int)fsize;
			}
			start = fsize;
			break;

		default:
			(void)posix_fileDeref(f);
			return -EINVAL;
	}

	oid = f->oid;
	(void)posix_fileDeref(f);

	/* resolve the requested range to an absolute [start,end) */
	start += lock.l_start;
	if (start < 0) {
		return -EINVAL;
	}

	if (lock.l_len > 0) {
		end = start + lock.l_len;
		if (end < start) {
			return -EINVAL; /* overflow */
		}
	}
	else if (lock.l_len == 0) {
		end = FLOCK_EOF; /* lock to end of file */
	}
	else {
		/* negative length: the range extends below l_start */
		end = start;
		start = start + lock.l_len;
		if (start < 0) {
			return -EINVAL;
		}
	}

	pid = process_getPid(proc_current()->process);

	if (cmd == (unsigned int)F_GETLK) {
		(void)proc_lockSet(&posix_common.fileLocksLock);
		conflict = _posix_lockConflict(oid, pid, start, end, type);
		if (conflict != NULL) {
			lock.l_type = conflict->type;
			lock.l_whence = SEEK_SET;
			lock.l_start = conflict->start;
			lock.l_len = (conflict->end == FLOCK_EOF) ? 0 : (conflict->end - conflict->start);
			lock.l_pid = conflict->pid;
		}
		else {
			lock.l_type = F_UNLCK;
		}
		(void)proc_lockClear(&posix_common.fileLocksLock);

		hal_memcpy(ulock, &lock, sizeof(lock));
		return EOK;
	}

	if (type == F_UNLCK) {
		spare1 = vm_kmalloc(sizeof(flock_t));
		if (spare1 == NULL) {
			return -ENOLCK;
		}
		(void)proc_lockSet(&posix_common.fileLocksLock);
		_posix_lockClearRange(oid, pid, start, end, spare1, &spareUsed);
		(void)proc_lockClear(&posix_common.fileLocksLock);
		if (spareUsed == 0) {
			vm_kfree(spare1);
		}
		return EOK;
	}

	/* F_SETLK / F_SETLKW acquiring a read/write lock: up to two records are
	 * needed (one for a possible split while clearing our own overlaps, one for
	 * the new lock). Allocate both up front so the mutation cannot fail
	 * half-way. */
	spare1 = vm_kmalloc(sizeof(flock_t));
	spare2 = vm_kmalloc(sizeof(flock_t));
	if ((spare1 == NULL) || (spare2 == NULL)) {
		if (spare1 != NULL) {
			vm_kfree(spare1);
		}
		if (spare2 != NULL) {
			vm_kfree(spare2);
		}
		return -ENOLCK;
	}

	for (;;) {
		(void)proc_lockSet(&posix_common.fileLocksLock);
		conflict = _posix_lockConflict(oid, pid, start, end, type);
		if (conflict == NULL) {
			_posix_lockClearRange(oid, pid, start, end, spare1, &spareUsed);
			spare2->oid = oid;
			spare2->pid = pid;
			spare2->start = start;
			spare2->end = end;
			spare2->type = type;
			LIST_ADD(&posix_common.fileLocks, spare2);
			(void)proc_lockClear(&posix_common.fileLocksLock);
			if (spareUsed == 0) {
				vm_kfree(spare1);
			}
			return EOK;
		}
		(void)proc_lockClear(&posix_common.fileLocksLock);

		if (cmd != (unsigned int)F_SETLKW) {
			vm_kfree(spare1);
			vm_kfree(spare2);
			return -EAGAIN;
		}

		/* F_SETLKW blocks until the conflict clears.
		 * TODO(fcntl-lock): replace this bounded poll with a wait queue woken
		 * by the unlock path, and make it interruptible (return -EINTR on a
		 * pending signal). v1 is a correct-but-crude 20 ms retry. */
		(void)proc_threadSleep(20000);
	}
}


int posix_fcntl(int fd, unsigned int cmd, u8 *ustack)
{
	TRACE("fcntl(%d, %u)", fd, cmd);

	int err = -EINVAL, fd2;
	unsigned int arg;

	switch (cmd) {
		case F_DUPFD_CLOEXEC:
		case F_DUPFD:
			GETFROMSTACK(ustack, int, fd2, 2U);
			err = posix_fcntlDup(fd, fd2, (cmd == (unsigned int)F_DUPFD_CLOEXEC) ? 1 : 0);
			break;

		case F_GETFD:
			err = posix_fcntlGetFd(fd);
			break;

		case F_SETFD:
			GETFROMSTACK(ustack, unsigned int, arg, 2U);
			err = posix_fcntlSetFd(fd, arg);
			break;

		case F_GETFL:
			err = posix_fcntlGetFl(fd);
			break;

		case F_SETFL:
			GETFROMSTACK(ustack, unsigned int, arg, 2U);
			err = posix_fcntlSetFl(fd, arg);
			break;

		case F_GETLK:
		case F_SETLK:
		case F_SETLKW: {
			struct flock *lockp;
			GETFROMSTACK(ustack, struct flock *, lockp, 2U);
			err = posix_fcntlLock(fd, cmd, lockp);
			break;
		}
		case F_GETOWN:
		case F_SETOWN:
		default:
			/* Handles any value of 'cmd' not covered by the case labels. */
			break;
	}

	return err;
}


static void ioctl_pack(msg_t *msg, unsigned long request, void *data, size_t size, oid_t *oid)
{
	ioctl_in_t *ioctl = (ioctl_in_t *)msg->i.raw;

	hal_memcpy(&msg->oid, oid, sizeof(*oid));
	msg->type = mtDevCtl;
	msg->i.data = NULL;
	msg->i.size = 0;
	msg->o.data = NULL;
	msg->o.size = 0;

	ioctl->request = request;
	ioctl->size = size;

	if ((request & IOC_INOUT) != 0U) {
		if ((request & IOC_IN) != 0U) {
			if (size <= (sizeof(msg->i.raw) - sizeof(ioctl_in_t))) {
				hal_memcpy(ioctl->data, data, size);
			}
			else {
				msg->i.data = data;
				msg->i.size = size;
			}
		}

		if (((request & IOC_OUT) != 0U) && (size > sizeof(msg->o.raw))) {
			msg->o.data = data;
			msg->o.size = size;
		}
	}
	else if (size > 0U) {
		/* the data is passed by value instead of pointer */
		size = min(size, sizeof(void *));
		hal_memcpy(ioctl->data, &data, size);
	}
	else {
		/* Nothing to do */
	}
}


static int ioctl_processResponse(const msg_t *msg, unsigned long request, void *data, size_t size)
{
	int err;

	err = msg->o.err;

	if (((request & IOC_OUT) != 0U) && (size <= sizeof(msg->o.raw))) {
		hal_memcpy(data, msg->o.raw, size);
	}

	return err;
}


int posix_ioctl(int fildes, unsigned long request, u8 *ustack)
{
	TRACE("ioctl(%d, %d)", fildes, request);

	open_file_t *f;
	int err;
	msg_t msg;
	void *data = NULL;
	size_t size = IOCPARM_LEN(request);

	err = posix_getOpenFile(fildes, &f);
	if (err == EOK) {
		/* TODO: handle POSIX defined requests */
		if (size > 0U) {
			GETFROMSTACK(ustack, void *, data, 2U);
			/* the actual size of the pointed-to structure: >= IOCPARM_LEN(request) */
			GETFROMSTACK(ustack, size_t, size, 3U);

			if ((request & IOC_INOUT) != 0U) {
				if (data == NULL) {
					err = -EFAULT;
				}
				else if (vm_mapBelongs(proc_current()->process, data, size) < 0) {
					err = -EFAULT;
				}
				else {
					/* Nothing to do */
				}
			}
		}

		if (err == EOK) {
			ioctl_pack(&msg, request, data, size, &f->oid);

			err = proc_send(f->oid.port, &msg);
			if (err == EOK) {
				err = ioctl_processResponse(&msg, request, data, size);
			}
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_socket(int domain, int type, int protocol)
{
	TRACE("socket(%d, %d, %d)", domain, type, protocol);

	process_info_t *p;
	open_file_t *f;
	int err, fd;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	fd = posix_newFile(p, 0, &f);
	if (fd < 0) {
		pinfo_put(p);
		return -EMFILE;
	}

	/* Write through f, not p->fds[fd].file: the construction reference keeps f
	 * alive, but the SLOT can be cleared by a racing close at any point below. */
	switch (domain) {
		case AF_UNIX:
			err = unix_socket(domain, (unsigned int)type, protocol);
			if (err >= 0) {
				f->type = ftUnixSocket;
				f->oid.port = US_PORT;
				f->oid.id = (unsigned int)err;
			}
			break;
		case AF_INET:
		case AF_INET6:
		case AF_KEY:
		case AF_PACKET:
			err = inet_socket(domain, type, protocol);
			if (err >= 0) {
				f->type = ftInetSocket;
				f->oid.port = (unsigned int)err;
				f->oid.id = 0U;
			}
			break;
		default:
			err = -EAFNOSUPPORT;
			break;
	}

	if (err < 0) {
		posix_fileConstructAbort(p, fd, f);
		pinfo_put(p);
		return err;
	}

	posix_fileConstructDone(p, fd, f, ((unsigned int)type & SOCK_CLOEXEC) != 0U);

	pinfo_put(p);
	return fd;
}


int posix_socketpair(int domain, int type, int protocol, int sv[2])
{
	TRACE("socketpair(%d, %d, %d, %p)", domain, type, protocol, sv);

	process_info_t *p;
	open_file_t *f0, *f1;
	int err, id[2];

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	if (domain != AF_UNIX) {
		pinfo_put(p);
		return -EAFNOSUPPORT;
	}

	sv[0] = posix_newFile(p, 0, &f0);
	if (sv[0] < 0) {
		pinfo_put(p);
		return -EMFILE;
	}

	sv[1] = posix_newFile(p, 0, &f1);
	if (sv[1] < 0) {
		posix_fileConstructAbort(p, sv[0], f0);
		pinfo_put(p);
		return -EMFILE;
	}

	err = unix_socketpair(domain, (unsigned int)type, protocol, id);
	if (err == 0) {
		f0->type = ftUnixSocket;
		f1->type = ftUnixSocket;
		f0->oid.port = US_PORT;
		f1->oid.port = US_PORT;
		f0->oid.id = (id_t)id[0];
		f1->oid.id = (id_t)id[1];

		posix_fileConstructDone(p, sv[0], f0, ((unsigned int)type & SOCK_CLOEXEC) != 0U);
		posix_fileConstructDone(p, sv[1], f1, ((unsigned int)type & SOCK_CLOEXEC) != 0U);
	}
	else {
		posix_fileConstructAbort(p, sv[1], f1);
		posix_fileConstructAbort(p, sv[0], f0);
	}

	pinfo_put(p);
	return err;
}


int posix_accept4(int socket, struct sockaddr *address, socklen_t *address_len, int flags)
{
	TRACE("accept4(%d, %s, %d)", socket, address == NULL ? NULL : address->sa_data, flags);

	process_info_t *p;
	open_file_t *f, *nf;
	int err, fd;

	p = pinfo_find(process_getPid(proc_current()->process));
	if (p == NULL) {
		return -1;
	}

	fd = posix_newFile(p, 0, &nf);
	if (fd < 0) {
		pinfo_put(p);
		return -EMFILE;
	}

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		/* Both accept calls BLOCK until a connection arrives, so nf is exposed
		 * in its half-built state for as long as that takes -- which is why it
		 * is written through the construction reference and never through
		 * p->fds[fd].file. */
		switch (f->type) {
			case ftInetSocket:
				err = inet_accept4(f->oid.port, address, address_len, (unsigned int)flags);
				if (err >= 0) {
					nf->type = ftInetSocket;
					nf->oid.port = (unsigned int)err;
					nf->oid.id = 0;
				}
				break;
			case ftUnixSocket:
				err = unix_accept4((unsigned int)f->oid.id, address, address_len, (unsigned int)flags);
				if (err >= 0) {
					nf->type = ftUnixSocket;
					nf->oid.port = US_PORT;
					nf->oid.id = (unsigned int)err;
				}
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	if (err < 0) {
		posix_fileConstructAbort(p, fd, nf);
		pinfo_put(p);
		return err;
	}

	posix_fileConstructDone(p, fd, nf, ((unsigned int)flags & SOCK_CLOEXEC) != 0U);

	pinfo_put(p);
	return fd;
}


int posix_accept(int socket, struct sockaddr *address, socklen_t *address_len)
{
	return posix_accept4(socket, address, address_len, 0);
}


int posix_bind(int socket, const struct sockaddr *address, socklen_t address_len)
{
	TRACE("bind(%d, %s)", socket, address == NULL ? NULL : address->sa_data);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_bind(f->oid.port, address, address_len);
				break;
			case ftUnixSocket:
				err = unix_bind((unsigned int)f->oid.id, address, address_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
	TRACE("connect(%d, %s)", socket, address == NULL ? NULL : address->sa_data);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_connect(f->oid.port, address, address_len);
				break;
			case ftUnixSocket:
				err = unix_connect((unsigned int)f->oid.id, address, address_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_uname(struct utsname *name)
{
	TRACE("uname()");

	(void)hal_strncpy(name->sysname, "Phoenix-RTOS", sizeof(name->sysname) - 1U);
	name->sysname[sizeof(name->sysname) - 1U] = '\0';
	(void)hal_strncpy(name->nodename, posix_common.hostname, sizeof(name->nodename) - 1U);
	name->nodename[sizeof(name->nodename) - 1U] = '\0';
	(void)hal_strncpy(name->release, RELEASE, sizeof(name->release) - 1U);
	name->release[sizeof(name->release) - 1U] = '\0';
	(void)hal_strncpy(name->version, VERSION, sizeof(name->version) - 1U);
	name->version[sizeof(name->version) - 1U] = '\0';
	(void)hal_strncpy(name->machine, TARGET_FAMILY, sizeof(name->machine) - 1U);
	name->machine[sizeof(name->machine) - 1U] = '\0';

	return 0;
}


int posix_gethostname(char *name, size_t namelen)
{
	TRACE("gethostname(%zu)", namelen);

	(void)hal_strncpy(name, posix_common.hostname, namelen);

	return 0;
}


int posix_getpeername(int socket, struct sockaddr *address, socklen_t *address_len)
{
	TRACE("getpeername(%d, %s)", socket, address == NULL ? NULL : address->sa_data);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_getpeername(f->oid.port, address, address_len);
				break;
			case ftUnixSocket:
				err = unix_getpeername((unsigned int)f->oid.id, address, address_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_getsockname(int socket, struct sockaddr *address, socklen_t *address_len)
{
	TRACE("getsockname(%d, %s)", socket, address == NULL ? NULL : address->sa_data);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_getsockname(f->oid.port, address, address_len);
				break;
			case ftUnixSocket:
				err = unix_getsockname((unsigned int)f->oid.id, address, address_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_getsockopt(int socket, int level, int optname, void *optval, socklen_t *optlen)
{
	TRACE("getsockopt(%d, %d, %d)", socket, level, optname);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_getsockopt(f->oid.port, level, optname, optval, optlen);
				break;
			case ftUnixSocket:
				err = unix_getsockopt((unsigned int)f->oid.id, level, optname, optval, optlen);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_listen(int socket, int backlog)
{
	TRACE("listen(%d, %d)", socket, backlog);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_listen(f->oid.port, backlog);
				break;
			case ftUnixSocket:
				err = unix_listen((unsigned int)f->oid.id, backlog);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


ssize_t posix_recvfrom(int socket, void *message, size_t length, int flags, struct sockaddr *src_addr, socklen_t *src_len)
{
	TRACE("recvfrom(%d, %d, %s)", socket, length, src_addr == NULL ? NULL : src_addr->sa_data);

	open_file_t *f;
	ssize_t err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_recvfrom(f->oid.port, message, length, (unsigned int)flags, src_addr, src_len);
				break;
			case ftUnixSocket:
				err = unix_recvfrom((unsigned int)f->oid.id, message, length, (unsigned int)flags, src_addr, src_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


ssize_t posix_sendto(int socket, const void *message, size_t length, int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
{
	TRACE("sendto(%d, %s, %d, %s)", socket, message, length, dest_addr == NULL ? NULL : dest_addr->sa_data);

	open_file_t *f;
	ssize_t err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_sendto(f->oid.port, message, length, (unsigned int)flags, dest_addr, dest_len);
				break;
			case ftUnixSocket:
				err = unix_sendto((unsigned int)f->oid.id, message, length, (unsigned int)flags, dest_addr, dest_len);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	if ((err == -EPIPE) && (((unsigned int)flags & MSG_NOSIGNAL) == 0U)) {
		posix_sigpipe();
	}

	return err;
}


ssize_t posix_recvmsg(int socket, struct msghdr *msg, int flags)
{
	TRACE("recvmsg(%d, %p, %d)", socket, msg, flags);

	open_file_t *f;
	ssize_t err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_recvmsg(f->oid.port, msg, (unsigned int)flags);
				break;
			case ftUnixSocket:
				err = unix_recvmsg((unsigned int)f->oid.id, msg, (unsigned int)flags);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


ssize_t posix_sendmsg(int socket, const struct msghdr *msg, int flags)
{
	TRACE("sendmsg(%d, %p, %d)", socket, msg, flags);

	open_file_t *f;
	ssize_t err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_sendmsg(f->oid.port, msg, (unsigned int)flags);
				break;
			case ftUnixSocket:
				err = unix_sendmsg((unsigned int)f->oid.id, msg, (unsigned int)flags);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	if ((err == -EPIPE) && (((unsigned int)flags & MSG_NOSIGNAL) == 0U)) {
		posix_sigpipe();
	}

	return err;
}


int posix_shutdown(int socket, int how)
{
	TRACE("shutdown(%d, %d)", socket, how);

	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_shutdown(f->oid.port, how);
				break;
			case ftUnixSocket:
				err = unix_shutdown((unsigned int)f->oid.id, how);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_sethostname(const char *name, size_t namelen)
{
	TRACE("sethostname(%zu)", namelen);

	if (namelen > HOST_NAME_MAX) {
		return -EINVAL;
	}

	(void)hal_strncpy(posix_common.hostname, name, namelen);
	posix_common.hostname[namelen] = '\0';

	return 0;
}


int posix_setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen)
{
	open_file_t *f;
	int err;

	err = posix_getOpenFile(socket, &f);
	if (err == 0) {
		switch (f->type) {
			case ftInetSocket:
				err = inet_setsockopt(f->oid.port, level, optname, optval, optlen);
				break;
			case ftUnixSocket:
				err = unix_setsockopt((unsigned int)f->oid.id, level, optname, optval, optlen);
				break;
			default:
				err = -ENOTSOCK;
				break;
		}

		(void)posix_fileDeref(f);
	}

	return err;
}


int posix_futimens(int fildes, const struct timespec *times)
{
	TRACE("futimens(%d)", fildes);

	open_file_t *f;
	msg_t msg;
	int err;
	time_t sec0, sec1, offs;

	if (times == NULL) {
		proc_gettime(&sec0, &offs);
		sec0 = (sec0 + offs) / (1000 * 1000);
		sec1 = sec0;
	}
	else {
		sec1 = times[1].tv_sec;
		sec0 = times[0].tv_sec;
	}

	err = posix_getOpenFile(fildes, &f);
	if (err < 0) {
		return err;
	}

	hal_memset(&msg, 0, sizeof(msg_t));

	msg.type = mtSetAttr;
	hal_memcpy(&msg.oid, &f->oid, sizeof(oid_t));

	msg.i.attr.type = atMTime;
	msg.i.attr.val = (long long)sec1;
	err = proc_send(f->oid.port, &msg);
	if ((err >= 0) && (msg.o.err >= 0)) {
		msg.i.attr.type = atATime;
		msg.i.attr.val = (long long)sec0;
		err = proc_send(f->oid.port, &msg);
	}
	if (err >= 0) {
		err = msg.o.err;
	}

	(void)posix_fileDeref(f);

	return err;
}


static int do_poll_iteration(struct pollfd *fds, nfds_t nfds, int *hasUnix, unsigned int block_ms)
{
	msg_t msg;
	int ready = 0;
	unsigned int i;
	int err;
	open_file_t *f;
	unsigned short events, revents;

	hal_memset(&msg, 0, sizeof(msg));

	msg.type = mtGetAttr;
	msg.i.attr.type = atPollStatus;

	for (i = 0; i < nfds; ++i) {
		if (fds[i].fd < 0) {
			continue;
		}
		events = (unsigned short)fds[i].events;
		revents = (unsigned short)fds[i].revents;

		/* Low 16 bits = event mask; high bits optionally carry a block timeout
		 * (ms) so a poll-status server MAY block until readiness instead of the
		 * caller spin-polling. Only posix_poll's single-ftInetSocket fast path
		 * passes block_ms > 0 (and only the lwip socket server decodes it); every
		 * other path passes 0, i.e. a bare mask (unchanged legacy snapshot). The
		 * AF_UNIX branch below uses the raw `events`, never this packed value. */
		msg.i.attr.val = (long long)events | ((long long)block_ms << 16);

		if (posix_getOpenFile(fds[i].fd, &f) < 0) {
			err = (int)POLLNVAL;
		}
		else {
			hal_memcpy(&msg.oid, &f->oid, sizeof(oid_t));
			(void)posix_fileDeref(f);

			if (f->type == ftUnixSocket) {
				if (hasUnix != NULL) {
					*hasUnix = 1;
				}
				err = unix_poll((unsigned int)msg.oid.id, events);
			}
			else {
				err = proc_send(msg.oid.port, &msg);
				if (err >= 0) {
					/* FIXME: 8 byte attr assigned to 4 byte err */
					err = (msg.o.err >= 0) ? (int)msg.o.attr.val : msg.o.err;
				}
			}
		}

		if (err == -EINTR) {
			return err;
		}

		if (err < 0) {
			revents |= POLLHUP;
		}
		else if (err > 0) {
			revents |= (unsigned short)err;
		}
		else {
			/* No action required */
		}

		revents &= ~(~events & (POLLIN | POLLOUT | POLLPRI | POLLRDNORM | POLLWRNORM | POLLRDBAND | POLLWRBAND));

		if (revents != 0U) {
			++ready;
		}

		fds[i].revents = (short)revents;
	}

	return ready;
}


int posix_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
	unsigned int i, n;
	int ready;
	int hasUnix = 0;
	int singleInet = 0;
	open_file_t *pf;
	time_t timeout, now, cur, t0, t1;

	n = 0U;

	for (i = 0U; i < nfds; ++i) {
		fds[i].revents = 0;
		if (fds[i].fd >= 0) {
			++n;
		}
	}

	if (n == 0U) {
		if (timeout_ms > 0) {
			(void)proc_threadSleep(timeout_ms * 1000LL);
		}
		return 0;
	}

	/* Fast-path gate: a poll on exactly ONE inet socket lets the socket server
	 * block until readiness (see do_poll_iteration's block_ms) instead of the
	 * kernel spin-polling every POLL_INTERVAL. Restricted to a single ftInetSocket
	 * fd so only the lwip socket server (which decodes the packed timeout) is
	 * affected; multi-fd and non-inet polls keep the legacy timed re-poll. */
	if (n == 1U) {
		for (i = 0U; i < nfds; ++i) {
			if (fds[i].fd < 0) {
				continue;
			}
			if (posix_getOpenFile(fds[i].fd, &pf) == 0) {
				if (pf->type == ftInetSocket) {
					singleInet = 1;
				}
				(void)posix_fileDeref(pf);
			}
			break;
		}
	}

	if (timeout_ms >= 0) {
		proc_gettime(&timeout, NULL);
		timeout += (timeout_ms == 0) ? 1 : timeout_ms * 1000LL;
	}
	else {
		timeout = 0;
	}

	ready = do_poll_iteration(fds, nfds, &hasUnix, 0);
	while (ready == 0) {
		proc_gettime(&cur, NULL);
		if (timeout != 0) {
			if (cur > timeout) {
				break;
			}

			now = timeout - cur;
			if (now > POLL_INTERVAL) {
				now = POLL_INTERVAL;
			}
		}
		else {
			now = POLL_INTERVAL;
		}

		/*
		 * If any watched fd is an AF_UNIX socket, block on the unix poll queue
		 * (woken the instant any unix socket changes readiness) with `now` as a
		 * timeout fallback — so local X client<->server round-trips are not gated
		 * by the re-check interval. Sets with no AF_UNIX fd keep the timed sleep
		 * (their readiness comes from a remote server via mtGetAttr, which has no
		 * readiness-wakeup yet).
		 */
		if (hasUnix != 0) {
			if (unix_pollWait(cur + now) == -EINTR) {
				return -EINTR;
			}
			ready = do_poll_iteration(fds, nfds, &hasUnix, 0);
		}
		else if (singleInet != 0) {
			/* Single inet socket: let the socket server block until readiness
			 * (up to `now`) — it returns the instant data arrives, instead of the
			 * kernel sleeping the whole interval blind. Belt: if it returns
			 * not-ready having blocked less than `now` (e.g. a server that ignores
			 * the packed timeout), sleep the remainder so this can never busy-loop. */
			proc_gettime(&t0, NULL);
			ready = do_poll_iteration(fds, nfds, &hasUnix, (unsigned int)(now / 1000));
			if (ready == 0) {
				proc_gettime(&t1, NULL);
				if ((t1 - t0) < now) {
					(void)proc_threadSleep(now - (t1 - t0));
				}
			}
		}
		else {
			(void)proc_threadSleep(now);
			ready = do_poll_iteration(fds, nfds, &hasUnix, 0);
		}
	}

	return ready;
}


static int posix_killOne(pid_t pid, int tid, int sig)
{
	process_info_t *pinfo;
	process_t *proc;
	thread_t *thr;
	int err;

	pinfo = pinfo_find(pid);
	if (pinfo == NULL) {
		return -ESRCH;
	}

	proc = proc_find(pinfo->process);
	if (proc == NULL) {
		pinfo_put(pinfo);
		return -ESRCH;
	}

	if (tid == 0) {
		err = threads_sigpost(proc, NULL, sig);
	}
	else {
		thr = threads_findThread(tid);
		if (thr == NULL) {
			(void)proc_put(proc);
			pinfo_put(pinfo);
			return -EINVAL;
		}

		if (thr->process == proc) {
			err = threads_sigpost(proc, thr, sig);
		}
		else {
			err = -EINVAL;
		}

		threads_put(thr);
	}
	(void)proc_put(proc);
	pinfo_put(pinfo);

	return err;
}


static int posix_killGroup(pid_t pgid, int sig)
{
	process_info_t *pinfo;
	rbnode_t *node;
	int err = -ESRCH;

	(void)proc_lockSet(&posix_common.lock);
	for (node = lib_rbMinimum(posix_common.pid.root); node != NULL; node = lib_rbNext(node)) {
		pinfo = lib_treeof(process_info_t, linkage, node);

		if (pinfo->pgid == pgid) {
			err = EOK;
			(void)proc_sigpost(pinfo->process, sig);
		}
	}
	(void)proc_lockClear(&posix_common.lock);

	return err;
}


int posix_tkill(pid_t pid, int tid, int sig)
{
	TRACE("tkill(%p, %d, %d)", pid, tid, sig);

	if ((sig < 0) || (sig > NSIG)) {
		return -EINVAL;
	}

	/* TODO: handle pid = 0 */
	if (pid == 0) {
		return -ENOSYS;
	}

	if (pid == -1) {
		return -ESRCH;
	}

	return (pid > 0) ? posix_killOne(pid, tid, sig) : posix_killGroup(-pid, sig);
}


void posix_sigchild(pid_t ppid)
{
	(void)posix_tkill(ppid, 0, SIGCHLD);
}


int posix_setpgid(pid_t pid, pid_t pgid)
{
	process_info_t *pinfo;

	if ((pid < 0) || (pgid < 0)) {
		return -EINVAL;
	}

	if (pid == 0) {
		pid = process_getPid(proc_current()->process);
	}

	if (pgid == 0) {
		pgid = pid;
	}

	pinfo = pinfo_find(pid);
	if (pinfo == NULL) {
		return -ESRCH;
	}

	(void)proc_lockSet(&pinfo->lock);
	pinfo->pgid = pgid;
	(void)proc_lockClear(&pinfo->lock);
	pinfo_put(pinfo);
	return EOK;
}


pid_t posix_getpgid(pid_t pid)
{
	process_info_t *pinfo;
	pid_t res;

	if (pid < 0) {
		return -EINVAL;
	}

	if (pid == 0) {
		pid = process_getPid(proc_current()->process);
	}

	pinfo = pinfo_find(pid);
	if (pinfo == NULL) {
		return -ESRCH;
	}

	(void)proc_lockSet(&pinfo->lock);
	res = pinfo->pgid;
	(void)proc_lockClear(&pinfo->lock);
	pinfo_put(pinfo);

	return res;
}


pid_t posix_setsid(void)
{
	process_info_t *pinfo;
	pid_t pid;

	pid = process_getPid(proc_current()->process);

	pinfo = pinfo_find(pid);
	if (pinfo == NULL) {
		return -EPERM;
	}

	/* FIXME (pedantic): Should check if any process has my group id */
	(void)proc_lockSet(&pinfo->lock);
	if (pinfo->pgid == pid) {
		(void)proc_lockClear(&pinfo->lock);
		pinfo_put(pinfo);
		return -EPERM;
	}

	pinfo->pgid = pid;
	(void)proc_lockClear(&pinfo->lock);
	pinfo_put(pinfo);

	return pid;
}


static int waitpid_isWaitValid(pid_t pid, process_info_t *parent, process_info_t *child)
{
	if (pid == -1) {
		return 1;
	}
	if ((pid == 0) && (child->pgid == parent->pgid)) {
		return 1;
	}
	if ((pid < 0) && (child->pgid == -pid)) {
		return 1;
	}
	return (pid == child->process) ? 1 : 0;
}


int posix_waitpid(pid_t child, int *status, unsigned int options)
{
	process_info_t *pinfo, *c;
	pid_t pid;
	int err = EOK, wnohang = 0;

	if (options != 0U) {
		if ((options & ~((unsigned int)(WNOHANG | WUNTRACED | WCONTINUED))) != 0U) {
			return -EINVAL;
		}

		/* TODO: handle WUNTRACED and WCONTINUED once SIGCONT/SIGSTOP gets implemented */

		wnohang = (options & WNOHANG) != 0U ? 1 : 0;
	}

	pid = process_getPid(proc_current()->process);

	pinfo = pinfo_find(pid);
	LIB_ASSERT_ALWAYS(pinfo != NULL, "pinfo not found, pid: %d", pid);

	(void)proc_lockSet(&pinfo->lock);
	for (;;) {
		/* Do this in the loop in case someone has a bad idea of doing multithreaded waitpid */
		err = -ECHILD;

		if (pinfo->zombies != NULL) {
			c = pinfo->zombies;
			do {
				if (waitpid_isWaitValid(child, pinfo, c) != 0) {
					LIST_REMOVE(&pinfo->zombies, c);
					err = c->process;
					if (status != NULL) {
						*status = c->exitcode;
					}
					(void)proc_lockClear(&pinfo->lock);

					pinfo_put(c);
					pinfo_put(pinfo);
					return err;
				}

				c = c->next;
			} while (c != pinfo->zombies);
		}

		if (pinfo->children != NULL) {
			c = pinfo->children;
			do {
				if (waitpid_isWaitValid(child, pinfo, c) != 0) {
					err = EOK;
					break;
				}
				c = c->next;
			} while (c != pinfo->children);
		}

		if ((err < 0) || (wnohang != 0)) {
			break;
		}

		err = proc_lockWait(&pinfo->wait, &pinfo->lock, 0);

		if (err == -EINTR) {
			/* pinfo->lock is clear */
			pinfo_put(pinfo);
			return -EINTR;
		}
		else if (err != 0) {
			/* Should not happen */
			break;
		}
		else {
			/* No action required */
		}
	}
	(void)proc_lockClear(&pinfo->lock);
	pinfo_put(pinfo);

	return err;
}


void posix_died(pid_t pid, int exit)
{
	process_info_t *pinfo, *ppinfo, *init, *cinfo, *zinfo, *zombies;
	int adopted = 1;

	pinfo = pinfo_find(pid);
	LIB_ASSERT_ALWAYS(pinfo != NULL, "pinfo not found, pid: %d", pid);

	init = pinfo_find(1);
	LIB_ASSERT_ALWAYS(init != NULL, "init not found");

	ppinfo = pinfo_find(pinfo->parent);

	(void)posix_exit(pinfo, exit);

	/* We might not find a parent if it died just now */
	if (ppinfo != NULL) {
		/* Make a zombie, wakeup waitpid */
		(void)proc_lockSet(&ppinfo->lock);
		/* Check if we didn't get adopted by the init in the meantime */
		if ((ppinfo != init) && (LIST_BELONGS(&ppinfo->children, pinfo) != 0)) {
			LIST_REMOVE(&ppinfo->children, pinfo);
			LIST_ADD(&ppinfo->zombies, pinfo);
			if (proc_threadBroadcast(&ppinfo->wait) == 0) {
				/* Signal parent because no one was waiting in waitpid() */
				posix_sigchild(pinfo->parent);
			}
			adopted = 0;
		}
		(void)proc_lockClear(&ppinfo->lock);
		pinfo_put(ppinfo);
	}

	(void)proc_lockSet2(&pinfo->lock, &init->lock);
	/* Collect all zombies */
	zombies = pinfo->zombies;
	pinfo->zombies = NULL;

	/* Adopt children */
	while (pinfo->children != NULL) {
		cinfo = pinfo->children;
		LIST_REMOVE(&pinfo->children, cinfo);
		/* Treat as atomic */
		cinfo->parent = 1;
		LIST_ADD(&init->children, cinfo);
	}

	if (adopted != 0) {
		LIB_ASSERT(LIST_BELONGS(&init->children, pinfo) != 0,
				"zombie's neither parent nor init child, pid: %d, ppid: %d", pid, pinfo->parent);
		/* We were adopted by the init at some point */
		LIST_REMOVE(&init->children, pinfo);
		LIST_ADD(&zombies, pinfo);
	}
	(void)proc_lockClear(&pinfo->lock);
	(void)proc_lockClear(&init->lock);
	pinfo_put(init);

	/* Reap all orphaned zombies */
	while (zombies != NULL) {
		zinfo = zombies;
		LIST_REMOVE(&zombies, zinfo);
		pinfo_put(zinfo);
	}

	pinfo_put(pinfo);
}


pid_t posix_getppid(pid_t pid)
{
	process_info_t *pinfo;
	int ret = 0;

	pinfo = pinfo_find(pid);
	if (pinfo == NULL) {
		return -ENOSYS;
	}

	ret = pinfo->parent;

	pinfo_put(pinfo);

	return ret;
}


void posix_init(void)
{
	(void)proc_lockInit(&posix_common.lock, &proc_lockAttrDefault, "posix.common");
	(void)proc_lockInit(&posix_common.fileLocksLock, &proc_lockAttrDefault, "posix.filelocks");
	lib_rbInit(&posix_common.pid, pinfo_cmp, NULL);
	unix_sockets_init();
	posix_common.fresh = 0;
	posix_common.fileLocks = NULL;
	hal_memset(posix_common.hostname, 0, sizeof(posix_common.hostname));
}
