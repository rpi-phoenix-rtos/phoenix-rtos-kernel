/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * CPU related routines
 *
 * Copyright 2014, 2017, 2018 Phoenix Systems
 * Author: Jacek Popko, Aleksander Kaminski, Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_HAL_CPU_H_
#define _PH_HAL_CPU_H_

#define SIG_SRC_SCHED 0
#define SIG_SRC_SCALL 1

#include <arch/cpu.h>
#include "spinlock.h"
#include "include/signal.h"


struct _hal_tls_t;


typedef ptr_t arg_t;


struct stackArg {
	const void *argp;
	size_t sz;
};

/* parasoft-begin-suppress MISRAC2012-RULE_1_5 MISRAC2012-RULE_8_8 "Implementations are arch-specific and often static-inlined in headers for performance reasons" */

/* interrupts */


void hal_cpuDisableInterrupts(void);


void hal_cpuEnableInterrupts(void);


/* performance */


void hal_cpuLowPower(time_t us, spinlock_t *spinlock, spinlock_ctx_t *sc);


int hal_cpuLowPowerAvail(void);


void hal_cpuSetDevBusy(int s);


void hal_cpuHalt(void);


/* parasoft-suppress-next-line MISRAC2012-RULE_8_6 "Definition in assembly code" */
void hal_cpuGetCycles(cycles_t *cb);


/* bit operations */

unsigned int hal_cpuGetLastBit(unsigned long v);


unsigned int hal_cpuGetFirstBit(unsigned long v);


/* context management */


void hal_cpuSetCtxGot(cpu_context_t *ctx, void *got);


void hal_cpuSetGot(void *got);


int hal_cpuCreateContext(cpu_context_t **nctx, startFn_t start, void *kstack, size_t kstacksz, void *ustack, void *arg, struct _hal_tls_t *tls);


/*
 * Perform a voluntary reschedule.
 * Function may be called under the `threads_common.spinlock`. In that case, pointer to this spinlock
 * must be given in the argument - the spinlock will be cleared once reschedule is performed.
 *
 * * spinlock - must be either NULL or `&threads_common.spinlock`.
 * * scp - pointer to spinlock context. Must not be NULL if `spinlock` is not NULL.
 */
/* parasoft-suppress-next-line MISRAC2012-RULE_8_6 "Definition in assembly code" */
int hal_cpuReschedule(struct _spinlock_t *spinlock, spinlock_ctx_t *scp);


void hal_cpuRestore(cpu_context_t *curr, cpu_context_t *next);


void hal_cpuSetReturnValue(cpu_context_t *ctx, void *retval);


/* parasoft-suppress-next-line MISRAC2012-RULE_8_6 "Definition in assembly code" */
void _hal_cpuSetKernelStack(void *kstack);


void *hal_cpuGetSP(cpu_context_t *ctx);


void *hal_cpuGetUserSP(cpu_context_t *ctx);


int hal_cpuSupervisorMode(cpu_context_t *ctx);


/* oldmask: mask to be restored in sigreturn after handling the signal
 * info, ss: NULL for a plain handler; for an SA_SIGINFO handler, its siginfo_t
 * and the uc_stack to report. Architectures without a ucontext_t ignore them. */
int hal_cpuPushSignal(void *kstack, void (*trampoline)(void), void (*handler)(int signo), cpu_context_t *signalCtx, int n, unsigned int oldmask, const int src, const siginfo_t *info, const stack_t *ss);


void hal_cpuSigreturn(void *kstack, void *ustack, cpu_context_t **ctx);


#ifdef _PH_HAVE_MCONTEXT
/* Return from an SA_SIGINFO handler: load ctx (the context saved on the kernel
 * stack at syscall entry) from the registers in uc, which the handler may have
 * changed, and from the rest of the saved signal context sctx. Both point into
 * user memory the caller has validated. Returns the value of the first argument
 * register, which the syscall return path writes back into ctx. */
void *hal_cpuSigreturnContext(cpu_context_t *ctx, const cpu_context_t *sctx, const ucontext_t *uc);
#endif


/* parasoft-suppress-next-line MISRAC2012-RULE_8_6 "Definition in assembly code" */
void hal_jmp(void *f, void *kstack, void *ustack, size_t kargc, const arg_t *kargs);


/* core management */


unsigned int hal_cpuGetID(void);


unsigned int hal_cpuGetCount(void);


char *hal_cpuInfo(char *info);


char *hal_cpuFeatures(char *features, size_t len);


void hal_cpuSendIPI(unsigned int cpu, unsigned int intr);


void hal_cpuBroadcastIPI(unsigned int intr);


__attribute__((noreturn)) void hal_cpuReboot(void);


void hal_cpuSmpSync(void);


/* thread local storage */


void hal_cpuTlsSet(struct _hal_tls_t *tls, cpu_context_t *ctx);


/* cache management */


void hal_cleanDCache(ptr_t start, size_t len);


/* stack management */


void hal_stackPutArgs(void **stackp, size_t argc, const struct stackArg *argv);

/* parasoft-end-suppress MISRAC2012-RULE_1_5 MISRAC2012-RULE_8_8 */
#endif
