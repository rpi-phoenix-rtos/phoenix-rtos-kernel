/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Private
 *
 * Copyright 2021, 2026 Phoenix Systems
 * Author: Pawel Pisarczyk, Ziemowit Leszczynski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_POSIX_POSIX_PRIVATE_H_
#define _PH_POSIX_POSIX_PRIVATE_H_

#include "hal/hal.h"
#include "proc/proc.h"
#include "posix.h"
#include "usocket.h"


#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGEMT    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGBUS    10
#define SIGSEGV   11
#define SIGSYS    12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGURG    16
#define SIGSTOP   17
#define SIGTSTP   18
#define SIGCONT   19
#define SIGCHLD   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGIO     23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGINFO   29
#define SIGUSR1   30
#define SIGUSR2   31

#define SIG_ERR (-1)
#define SIG_DFL (-2)
#define SIG_IGN (-3)

#define HOST_NAME_MAX 255U

/* Placed in a new file's oid.port while it is being constructed. Must be a port
 * id that can never be allocated (they come from an idtree seeded at 0) and must
 * differ from US_PORT, which this file uses to mean "AF_UNIX socket". */
#define POSIX_PORT_CONSTRUCTING 0xfffffffeU


enum { ftRegular,
	ftPipe,
	ftFifo,
	ftInetSocket,
	ftUnixSocket,
	ftTty,
	/* A file published in an fd slot but not yet filled in -- see the note on
	 * files under construction in posix.c. Appended, not inserted, so the
	 * existing values keep their numbers. Every test on f->type is either an
	 * equality against a real type or a switch with a default, so a file in
	 * this state is rejected everywhere rather than being mistaken for
	 * ftRegular (value 0), which is what a zeroed struct used to leave behind
	 * -- and is why F_SEEKABLE() accepted a half-built file. */
	ftConstructing };


/* FIXME: share with posixsrv */
enum { pxBufferedPipe,
	pxPipe,
	pxPTY };


#define F_SEEKABLE(type) ((type) == ftRegular)


typedef struct {
	oid_t ln;
	oid_t oid;
	int refs;
	off_t offset;
	unsigned int status;
	lock_t lock;
	int type;
	char *path; /* canonical abs path captured at open (for fchdir); NULL if none */
	usocket_t *sock; /* ftUnixSocket: reference to the socket */
} open_file_t;


typedef struct {
	open_file_t *file;
	unsigned int flags;
} fildes_t;


typedef struct _process_info_t {
	rbnode_t linkage;
	int process;
	int parent;
	int refs;
	int exitcode;

	thread_t *wait;

	struct _process_info_t *children;
	struct _process_info_t *zombies;
	struct _process_info_t *next, *prev;

	pid_t pgid;
	lock_t lock;
	int maxfd;
	int fdsz;
	fildes_t *fds;
} process_info_t;


int posix_fileDeref(open_file_t *f);


int posix_getOpenFile(int fd, open_file_t **f);


int posix_newFile(process_info_t *p, int fd, open_file_t **file);


int _posix_addOpenFile(process_info_t *p, open_file_t *f, unsigned int flags);


process_info_t *pinfo_find(int pid);


void pinfo_put(process_info_t *p);


int inet_accept4(unsigned int socket, struct sockaddr *address, socklen_t *address_len, unsigned int flags);


int inet_bind(unsigned int socket, const struct sockaddr *address, socklen_t address_len);


int inet_connect(unsigned int socket, const struct sockaddr *address, socklen_t address_len);


int inet_getpeername(unsigned int socket, struct sockaddr *address, socklen_t *address_len);


int inet_getsockname(unsigned int socket, struct sockaddr *address, socklen_t *address_len);


int inet_getsockopt(unsigned int socket, int level, int optname, void *optval, socklen_t *optlen);


int inet_listen(unsigned int socket, int backlog);


ssize_t inet_recvfrom(unsigned int socket, void *message, size_t length, unsigned int flags, struct sockaddr *src_addr, socklen_t *src_len);


ssize_t inet_sendto(unsigned int socket, const void *message, size_t length, unsigned int flags, const struct sockaddr *dest_addr, socklen_t dest_len);


ssize_t inet_recvmsg(unsigned int socket, struct msghdr *msg, unsigned int flags);


ssize_t inet_sendmsg(unsigned int socket, const struct msghdr *msg, unsigned int flags);


int inet_socket(int domain, int type, int protocol);


int inet_shutdown(unsigned int socket, int how);


int inet_setsockopt(unsigned int socket, int level, int optname, const void *optval, socklen_t optlen);


int inet_setfl(unsigned int socket, unsigned int flags);


int inet_getfl(unsigned int socket);


#endif
