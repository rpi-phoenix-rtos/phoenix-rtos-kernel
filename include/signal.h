/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Signals
 *
 * Copyright 2026 Phoenix Systems
 * Author: Jan Sikorski, Aleksander Kaminski, Jakub Klimek
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#ifndef _PH_SIGNAL_H_
#define _PH_SIGNAL_H_

#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef void (*sighandler_t)(int signo);


#define SIGNULL   0
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
#define SIGCANCEL 32 /* custom Phoenix-RTOS thread-termination signal */

#define NSIG       32 /* Count of standard POSIX signals */
#define NSIG_TOTAL 33 /* Count of all signals, including custom Phoenix-RTOS */

/* clang-format off */
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)


enum { SIG_BLOCK, SIG_SETMASK, SIG_UNBLOCK };
/* clang-format on */

#define SA_NOCLDSTOP (1U << 0) /* FIXME: honor once process stopping is implemented */
#define SA_NOCLDWAIT (1U << 1) /* FIXME: implement */
#define SA_NODEFER   (1U << 2)
#define SA_ONSTACK   (1U << 3) /* FIXME: sigaltstack() works on aarch64 only */
#define SA_RESETHAND (1U << 4)
#define SA_RESTART   (1U << 5) /* FIXME: implement */
#define SA_SIGINFO   (1U << 6) /* FIXME: siginfo_t and ucontext_t are delivered on aarch64 only */


/* si_code: generic values (same as Linux, so code testing SI_FROMUSER-style `si_code <= 0` works) */
#define SI_USER    0      /* kill() */
#define SI_KERNEL  0x80   /* raised by the kernel */
#define SI_QUEUE   (-1)   /* sigqueue() */
#define SI_TIMER   (-2)   /* timer expiration */
#define SI_MESGQ   (-3)   /* message queue state change */
#define SI_ASYNCIO (-4)   /* asynchronous I/O completion */
#define SI_TKILL   (-6)   /* tkill(), pthread_kill(), raise() */

/* si_code: SIGILL */
#define ILL_ILLOPC 1 /* illegal opcode */
#define ILL_ILLOPN 2 /* illegal operand */
#define ILL_ILLADR 3 /* illegal addressing mode */
#define ILL_ILLTRP 4 /* illegal trap */
#define ILL_PRVOPC 5 /* privileged opcode */
#define ILL_PRVREG 6 /* privileged register */
#define ILL_COPROC 7 /* coprocessor error */
#define ILL_BADSTK 8 /* internal stack error */

/* si_code: SIGFPE */
#define FPE_INTDIV 1 /* integer divide by zero */
#define FPE_INTOVF 2 /* integer overflow */
#define FPE_FLTDIV 3 /* floating-point divide by zero */
#define FPE_FLTOVF 4 /* floating-point overflow */
#define FPE_FLTUND 5 /* floating-point underflow */
#define FPE_FLTRES 6 /* floating-point inexact result */
#define FPE_FLTINV 7 /* invalid floating-point operation */
#define FPE_FLTSUB 8 /* subscript out of range */

/* si_code: SIGSEGV */
#define SEGV_MAPERR 1 /* address not mapped */
#define SEGV_ACCERR 2 /* invalid permissions for mapped object */

/* si_code: SIGBUS */
#define BUS_ADRALN 1 /* invalid address alignment */
#define BUS_ADRERR 2 /* nonexistent physical address */
#define BUS_OBJERR 3 /* object-specific hardware error */

/* si_code: SIGTRAP */
#define TRAP_BRKPT 1 /* process breakpoint */
#define TRAP_TRACE 2 /* process trace trap */

/* si_code: SIGCHLD */
#define CLD_EXITED    1 /* child has exited */
#define CLD_KILLED    2 /* child has terminated abnormally and did not create a core file */
#define CLD_DUMPED    3 /* child has terminated abnormally and created a core file */
#define CLD_TRAPPED   4 /* traced child has trapped */
#define CLD_STOPPED   5 /* child has stopped */
#define CLD_CONTINUED 6 /* stopped child has continued */

/* si_code: SIGPOLL */
#define POLL_IN  1 /* data input available */
#define POLL_OUT 2 /* output buffers available */
#define POLL_MSG 3 /* input message available */
#define POLL_ERR 4 /* I/O error */
#define POLL_PRI 5 /* high priority input available */
#define POLL_HUP 6 /* device disconnected */


typedef unsigned int sigset_t;
typedef int sig_atomic_t;


union sigval {
	int sival_int;
	void *sival_ptr;
};


typedef struct {
	int si_signo;
	int si_code;
	pid_t si_pid;
	uid_t si_uid;
	void *si_addr;
	int si_status;
	union sigval si_value;
	int si_errno; /* appended: the fields above keep their offsets */
} siginfo_t;


typedef struct {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
} stack_t;

/* stack_t ss_flags */
#define SS_ONSTACK 1 /* the thread is executing on the alternate signal stack */
#define SS_DISABLE 2 /* the alternate signal stack is disabled */

#define MINSIGSTKSZ 4096  /* smallest alternate signal stack */
#define SIGSTKSZ    16384 /* recommended alternate signal stack size */


struct sigaction {
	union {
		sighandler_t sa_handler;
		void (*sa_sigaction)(int signo, siginfo_t *info, void *context);
	};
	sigset_t sa_mask;
	int sa_flags;
};


#if defined(__aarch64__)
#include "arch/aarch64/ucontext.h"
#endif

#ifdef _PH_HAVE_MCONTEXT

/* The context an SA_SIGINFO handler receives as its third argument. Register
 * values written into uc_mcontext, and uc_sigmask, take effect when the
 * handler returns. */
typedef struct ucontext {
	unsigned long uc_flags;
	struct ucontext *uc_link;
	stack_t uc_stack;
	sigset_t uc_sigmask;
	mcontext_t uc_mcontext;
} ucontext_t;

#endif

#ifdef __cplusplus
}
#endif

#endif
