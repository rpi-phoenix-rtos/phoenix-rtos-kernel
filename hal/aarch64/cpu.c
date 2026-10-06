/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * CPU related routines
 *
 * Copyright 2014, 2017, 2018, 2024 Phoenix Systems
 * Author: Jacek Popko, Aleksander Kaminski, Pawel Pisarczyk, Jacek Maksymowicz
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/cpu.h"
#include "hal/string.h"
#include "hal/spinlock.h"
#include "hal/hal.h"

#include "aarch64.h"
#include "config.h"

#define CONST_STR_SIZE(x) (x), (sizeof(x) - 1U)

/* Function creates new cpu context on top of given thread kernel stack */
int hal_cpuCreateContext(cpu_context_t **nctx, startFn_t start, void *kstack, size_t kstacksz, void *ustack, void *arg, hal_tls_t *tls)
{
	cpu_context_t *ctx;
	size_t i;

	(void)tls;

	*nctx = NULL;
	if (kstack == NULL) {
		return -1;
	}

	kstacksz &= ~0xfUL;

	if (kstacksz < sizeof(cpu_context_t)) {
		return -1;
	}

	/* Align user stack to 16 bytes */
	ustack = (void *)((ptr_t)ustack & ~0xfUL);

	/* Prepare initial kernel stack */
	ctx = (cpu_context_t *)(kstack + kstacksz - sizeof(cpu_context_t));

	/* Set all registers to NAN */
	for (i = 0; i < 64U; i += 2U) {
		ctx->freg[i] = ~0UL;
		ctx->freg[i + 1U] = ~0UL;
	}

	ctx->fpsr = 0;
	ctx->fpcr = 0;
	ctx->cpacr = 0;

	ctx->x[0] = (u64)arg;
	for (i = 1; i < 31U; i++) {
		ctx->x[i] = 0x0101010101010101UL * i;
	}

	/* Enable interrupts, set normal execution mode */
	/* parasoft-suppress-next-line MISRAC2012-RULE_11_1 "Need to assign function address to processor register" */
	ctx->pc = (u64)start;

	if (ustack != NULL) {
		ctx->psr = MODE_EL0;
		ctx->sp = (u64)ustack;
	}
	else {
		ctx->psr = MODE_EL1_SP1;
		ctx->sp = (u64)kstack + kstacksz;
	}

	ctx->x[29] = ctx->sp;
	*nctx = ctx;

	return 0;
}


/* An SA_SIGINFO frame adds a ucontext_t and a siginfo_t between the saved
 * context and the trampoline arguments. threads.c bounds the frame below
 * signalCtx by one more cpu_context_t, so the additions must fit in that. */
_Static_assert(sizeof(ucontext_t) + sizeof(siginfo_t) + (12U * sizeof(u64)) + 32U <= sizeof(cpu_context_t),
		"SA_SIGINFO signal frame exceeds the bound checked by the scheduler");


#define PSR_NZCV 0xf0000000UL


/*
 * Signal frame, from the interrupted (or alternate) stack downwards:
 *
 *   cpu_context_t  signalCtx: the interrupted context; restored by sigreturn
 *   ucontext_t     uc        (SA_SIGINFO only) the handler's copy of it
 *   siginfo_t      si        (SA_SIGINFO only)
 *   arguments      n, handler, oldmask, signalCtx, pc, sp, psr, si[, uc]
 *
 * n is at the lowest address, where the trampoline finds it. The word after psr
 * is NULL for a plain handler; it used to be alignment padding, so the plain
 * frame keeps its size and layout and older trampolines, which read only the
 * first seven words, handle both frames.
 */
int hal_cpuPushSignal(void *kstack, void (*trampoline)(void), void (*handler)(int signo), cpu_context_t *signalCtx, int n, unsigned int oldmask, const int src, const siginfo_t *info, const stack_t *ss)
{
	cpu_context_t *ctx = (void *)((char *)kstack - sizeof(cpu_context_t));
	ucontext_t *uc = NULL;
	siginfo_t *si = NULL;
	size_t i;
	const struct stackArg args[] = {
		{ &uc, sizeof(uc) },
		{ &si, sizeof(si) },
		{ &ctx->psr, sizeof(ctx->psr) },
		{ &ctx->sp, sizeof(ctx->sp) },
		{ &ctx->pc, sizeof(ctx->pc) },
		{ &signalCtx, sizeof(signalCtx) },
		{ &oldmask, sizeof(oldmask) },
		{ &handler, sizeof(handler) },
		{ &n, sizeof(n) },
	};
	size_t argc = sizeof(args) / sizeof(args[0]);

	(void)src;

	if (info != NULL) {
		uc = (void *)(((ptr_t)signalCtx - sizeof(*uc)) & ~(ptr_t)0xfU);
		si = (void *)(((ptr_t)uc - sizeof(*si)) & ~(ptr_t)0xfU);

		hal_memcpy(si, info, sizeof(*si));

		hal_memset(uc, 0, sizeof(*uc));
		hal_memcpy(&uc->uc_stack, ss, sizeof(uc->uc_stack));
		uc->uc_sigmask = oldmask;
		uc->uc_mcontext.fault_address = (u64)(ptr_t)info->si_addr;
		for (i = 0; i < 31U; i++) {
			uc->uc_mcontext.regs[i] = ctx->x[i];
		}
		uc->uc_mcontext.sp = ctx->sp;
		uc->uc_mcontext.pc = ctx->pc;
		uc->uc_mcontext.pstate = ctx->psr;
	}
	else {
		/* Plain frame: leave out uc, keep si as the NULL marker */
		argc--;
	}

	hal_memcpy(signalCtx, ctx, sizeof(cpu_context_t));

	/* parasoft-suppress-next-line MISRAC2012-RULE_11_1 "Program counter must be set to the address of the function" */
	signalCtx->pc = (u64)trampoline;
	signalCtx->sp = (si != NULL) ? (u64)(ptr_t)si : (u64)(ptr_t)signalCtx;

	hal_stackPutArgs((void **)&signalCtx->sp, argc, &args[sizeof(args) / sizeof(args[0]) - argc]);

	return 0;
}


void hal_cpuSigreturn(void *kstack, void *ustack, cpu_context_t **ctx)
{
	(void)kstack;
	GETFROMSTACK(ustack, u64, (*ctx)->pc, 2);
	GETFROMSTACK(ustack, u64, (*ctx)->sp, 3);
	GETFROMSTACK(ustack, u64, (*ctx)->psr, 4);
}


void *hal_cpuSigreturnContext(cpu_context_t *ctx, const cpu_context_t *sctx, const ucontext_t *uc)
{
	size_t i;

#ifndef __SOFTFP__
	/* Only the FPU enable bits are restored: the FP/SIMD registers below are
	 * loaded on return only when both are set. */
	ctx->cpacr = sctx->cpacr & (3UL << 20);
	ctx->fpcr = sctx->fpcr;
	ctx->fpsr = sctx->fpsr;
	hal_memcpy(ctx->freg, sctx->freg, sizeof(ctx->freg));
#else
	(void)sctx;
#endif

	for (i = 0; i < 31U; i++) {
		ctx->x[i] = uc->uc_mcontext.regs[i];
	}
	ctx->sp = uc->uc_mcontext.sp;
	ctx->pc = uc->uc_mcontext.pc;

	/* The handler may change the condition flags only: never the exception
	 * masks (an EL0 thread with IRQs masked could not be preempted) or the
	 * exception level. */
	ctx->psr = (uc->uc_mcontext.pstate & PSR_NZCV) | MODE_EL0;

	return (void *)(ptr_t)ctx->x[0];
}


static void appendToString(const char *in, size_t inLen, char *out, size_t *n, size_t limit)
{
	if ((*n + inLen) >= limit) {
		return;
	}

	(void)hal_strcpy(&out[*n], in);
	*n += inLen;
}


void hal_cpuGetProcID(struct aarch64_proc_id *out)
{
	out->mmfr0 = sysreg_read(id_aa64mmfr0_el1);
	out->pfr0 = sysreg_read(id_aa64pfr0_el1);
	out->isar0 = sysreg_read(id_aa64isar0_el1);
	out->dfr0 = (u32)sysreg_read(id_aa64dfr0_el1);
	out->midr = (u32)sysreg_read(midr_el1);
}


char *hal_cpuInfo(char *info)
{
	size_t n = 0;
	unsigned int cpuCount = hal_cpuGetCount();
	struct aarch64_proc_id procId;

	hal_cpuGetProcID(&procId);
	appendToString(CONST_STR_SIZE(HAL_NAME_PLATFORM), info, &n, 128);


	if (((procId.midr >> 16) & 0xfU) == 0xfU) {
		appendToString(CONST_STR_SIZE("ARMv8 "), info, &n, 128);
	}

	if (((procId.midr >> 4) & 0xfffU) == 0xd03U) {
		appendToString(CONST_STR_SIZE("Cortex-A53 "), info, &n, 128);
	}

	info[n++] = 'r';
	info[n++] = '0' + ((procId.midr >> 20) & 0xfU);
	info[n++] = 'p';
	info[n++] = '0' + (procId.midr & 0xfU);

	info[n++] = ' ';
	info[n++] = 'x';
	if (cpuCount >= 10U) {
		info[n++] = '0' + (cpuCount / 10U);
	}

	info[n++] = '0' + (cpuCount % 10U);

	info[n] = '\0';

	return info;
}


char *hal_cpuFeatures(char *features, size_t len)
{
	size_t n = 0;
	struct aarch64_proc_id procId;

	hal_cpuGetProcID(&procId);
	if (len == 0U) {
		return features;
	}

	if (((procId.pfr0 >> 12) & 0xfU) != 0U) {
		appendToString(CONST_STR_SIZE("EL3, "), features, &n, len);
	}

	if (((procId.pfr0 >> 8) & 0xfU) != 0U) {
		appendToString(CONST_STR_SIZE("EL2, "), features, &n, len);
	}

	switch ((procId.pfr0 >> 16) & 0xfU) {
		case 0:
			appendToString(CONST_STR_SIZE("FP, "), features, &n, len);
			break;

		case 1:
			appendToString(CONST_STR_SIZE("FP16, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.pfr0 >> 20) & 0xfU) {
		case 0: /* Fall-through */
		case 1:
			appendToString(CONST_STR_SIZE("AdvSIMD, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.isar0 >> 4) & 0xfU) {
		case 1: /* Fall-through */
		case 2:
			appendToString(CONST_STR_SIZE("AES, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.isar0 >> 8) & 0xfU) {
		case 1:
			appendToString(CONST_STR_SIZE("SHA1, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.isar0 >> 12) & 0xfU) {
		case 1:
			appendToString(CONST_STR_SIZE("SHA256, "), features, &n, len);
			break;

		case 2:
			appendToString(CONST_STR_SIZE("SHA512, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.isar0 >> 16) & 0xfU) {
		case 1:
			appendToString(CONST_STR_SIZE("CRC32, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	switch ((procId.isar0 >> 20) & 0xfU) {
		case 2: /* Fall-through */
		case 3:
			appendToString(CONST_STR_SIZE("LSE, "), features, &n, len);
			break;

		default:
			/* No action required */
			break;
	}

	if (n > 0U) {
		features[n - 2U] = '\0';
	}
	else {
		features[0] = '\0';
	}

	return features;
}


void hal_cpuTlsSet(hal_tls_t *tls, cpu_context_t *ctx)
{
	/* In theory there should be 16-byte thread control block but
	 * it's stored elsewhere so we need to subtract 16 from the pointer
	 */
	ptr_t ptr = tls->tls_base - 16U;
	sysreg_write(tpidr_el0, ptr);
	hal_cpuDataSyncBarrier();
}


void _hal_cpuSetKernelStack(void *kstack)
{
	hal_cpuDataSyncBarrier();
	sysreg_write(tpidr_el1, kstack);
	hal_cpuDataSyncBarrier();
}


void hal_cpuGetCycles(cycles_t *cb)
{
	*cb = sysreg_read(pmccntr_el0);
}


/* parasoft-suppress-next-line MISRAC2012-DIR_4_3 "Assembly is required for low-level operations" */
int hal_cpuCanRead(ptr_t va)
{
	u64 par, saved;

	/*
	 * Ask the MMU (`at s1e1r`) instead of walking the tables: no lock is taken, so this works in
	 * interrupt context. PAR_EL1 is saved and restored as the interrupted code may be between
	 * its own `at` and the read of PAR_EL1. The A72 has no PAN, so EL0 pages read fine from EL1.
	 */
	saved = sysreg_read(par_el1);
	/* clang-format off */
	__asm__ volatile ("at s1e1r, %0\n isb" : : "r"(va));
	/* clang-format on */
	par = sysreg_read(par_el1);
	sysreg_write(par_el1, saved);

	if ((par & 1U) != 0U) {
		return 0;
	}

	/* PAR_EL1.ATTR is a MAIR attribute: 0b0000xxxx is Device memory, where a read may have side effects */
	return ((par >> 60) != 0U) ? 1 : 0;
}


unsigned int hal_cpuBacktrace(ptr_t fp, ptr_t lo, ptr_t hi, u64 *ret, unsigned int n)
{
	unsigned int depth = 0;
	ptr_t page = ~(ptr_t)0, next; /* no page probed yet: never equal to a page address */

	/* A frame record is {previous fp, return address} at fp: 16 bytes, 16-aligned, in one page */
	while ((depth < n) && (fp >= lo) && (fp < hi) && ((hi - fp) >= 16U) && ((fp & 0xfU) == 0U)) {
		if ((fp & ~((ptr_t)SIZE_PAGE - 1U)) != page) {
			page = fp & ~((ptr_t)SIZE_PAGE - 1U);
			if (hal_cpuCanRead(page) == 0) {
				break;
			}
		}

		next = *(volatile ptr_t *)fp;
		ret[depth++] = *(volatile u64 *)(fp + 8U);
		if (next <= fp) {
			break; /* the chain must ascend the stack */
		}
		fp = next;
	}

	return depth;
}


/* Value-trap window read by exceptions_watchpointHandler (see exceptions.c).
 * trapHi == 0 means "halt on any store". */
addr_t hal_wpTrapLo = 0;
addr_t hal_wpTrapHi = 0;


void hal_cpuWatchpointSet(addr_t va, int enable, addr_t trapLo, addr_t trapHi)
{
	/* MDSCR_EL1.MDE = bit 15 (monitor-mode debug enable). */
	const unsigned long mdscrMde = (1UL << 15);

	hal_wpTrapLo = trapLo;
	hal_wpTrapHi = trapHi;

	if (enable == 0) {
		sysreg_write(dbgwcr0_el1, 0UL);
		sysreg_write(mdscr_el1, sysreg_read(mdscr_el1) & ~mdscrMde);
		hal_cpuInstrBarrier();
		return;
	}

	/* Clear the OS lock so debug exceptions can be generated (EL3/firmware may
	 * leave it set). */
	sysreg_write(oslar_el1, 0UL);
	sysreg_write(mdscr_el1, sysreg_read(mdscr_el1) | mdscrMde);

	/* Watched doubleword address (DBGWVR is 8-byte aligned). */
	sysreg_write(dbgwvr0_el1, va & ~7UL);

	/* DBGWCR0_EL1: E[0]=1 enable; PAC[2:1]=0b11 match EL0+EL1; LSC[4:3]=0b10
	 * store-only; BAS[12:5]=0xFF all 8 bytes; HMC[13]=0, SSC[15:14]=0,
	 * MASK[20:16]=0 (no range). (ARM ARM D2: DBGWCRn_EL1.) */
	const unsigned long wcr = (1UL << 0) | (0x3UL << 1) | (0x2UL << 3) | (0xFFUL << 5);
	sysreg_write(dbgwcr0_el1, wcr);
	hal_cpuInstrBarrier();
}


void hal_cpuLowPower(time_t us, spinlock_t *spinlock, spinlock_ctx_t *sc)
{
	hal_spinlockClear(spinlock, sc);
	hal_cpuHalt();
}


int hal_cpuLowPowerAvail(void)
{
	return 0;
}


/* cache management */


void hal_cleanDCache(ptr_t start, size_t len)
{
	hal_cpuCleanDataCache(start, start + len);
}
