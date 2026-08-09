/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Exception handling
 *
 * Copyright 2024 Phoenix Systems
 * Author: Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/exceptions.h"
#include "hal/spinlock.h"
#include "hal/cpu.h"
#include "hal/console.h"
#include "hal/string.h"
#include "include/mman.h"


/* Set to 1 to print text descriptions of exceptions for architecture extensions */
#define EXTENSION_DESCRIPTIONS 0U
#define N_EXCEPTIONS           64U

static struct {
	excHandlerFn_t handler[N_EXCEPTIONS];
	excHandlerFn_t defaultHandler;
	spinlock_t lock;
} exceptions;


static void exceptions_trampoline(unsigned int n, exc_context_t *ctx)
{
	exceptions.defaultHandler(n, ctx);
}


static const char *exceptionClassStr(unsigned int excClass)
{
	switch (excClass) {
		case EXC_UNDEFINED:
			return "Unknown reason";
		case EXC_TRAP_WFI_WFE:
			return "Trapped WFI/WFE";
		case EXC_TRAP_MCR_MRC_CP15:
			return "Trapped MCR/MRC access (cp15)";
		case EXC_TRAP_MCRR_MRRC_CP15:
			return "Trapped MCRR/MRRC access (cp15)";
		case EXC_TRAP_MCR_MRC_CP14:
			return "Trapped MCR/MRC access (cp14)";
		case EXC_TRAP_LDC_STC:
			return "Trapped LDC/STC access";
		case 7U:
			return "Trapped SME, SVE, Advanced SIMD or floating-point functionality due to CPACR_ELx.FPEN";
		case EXC_TRAP_MRRC_CP14:
			return "Trapped MRRC access (cp14)";
		case EXC_ILLEGAL_EXEC_STATE:
			return "Illegal Execution state";
		case EXC_SVC_AA32:
			return "SVC (AA32)";
		case EXC_TRAP_MSRR_MRRS_SYS_AA64:
			return "Trapped MSRR/MRRS/SYS (AA64)";
		case 21U:
			return "SVC (AA64)";
		case EXC_TRAP_MSR_MRS_SYS_AA64:
			return "Trapped MSR/MRS/SYS (AA64)";
		case EXC_INSTR_ABORT_EL0:
			return "Instruction Abort (EL0)";
		case EXC_INSTR_ABORT_EL1:
			return "Instruction Abort (EL1)";
		case EXC_PC_ALIGN:
			return "PC alignment fault";
		case EXC_DATA_ABORT_EL0:
			return "Data Abort (EL0)";
		case EXC_DATA_ABORT_EL1:
			return "Data Abort (EL1)";
		case EXC_SP_ALIGN:
			return "SP alignment fault";
		case EXC_TRAP_FPU_AA32:
			return "Trapped floating-point exception (AA32)";
		case EXC_TRAP_FPU_AA64:
			return "Trapped floating-point exception (AA64)";
		case EXC_SERROR:
			return "SError exception";
		case EXC_BREAKPOINT_EL0:
			return "Breakpoint (EL0)";
		case EXC_BREAKPOINT_EL1:
			return "Breakpoint (EL1)";
		case EXC_STEP_EL0:
			return "Software Step (EL0)";
		case EXC_STEP_EL1:
			return "Software Step (EL1)";
		case EXC_WATCHPOINT_EL0:
			return "Watchpoint (EL0)";
		case EXC_WATCHPOINT_EL1:
			return "Watchpoint (EL1)";
		case EXC_BKPT_AA32:
			return "BKPT (AA32)";
		case EXC_BRK_AA64:
			return "BRK (AA64)";
#if EXTENSION_DESCRIPTIONS
		case 10U:
			return "(FEAT_LS64) Trapped execution of an LD64B or ST64B* instruction";
		case 13U:
			return "(FEAT_BTI) Branch Target Exception";
		case 25U:
			return "(FEAT_SVE) Access to SVE functionality trapped";
		case 27U:
			return "(FEAT_TME) Exception from an access to a TSTART instruction...";
		case 28U:
			return "(FEAT_FPAC) Exception from a PAC Fail";
		case 29U:
			return "(FEAT_SME) Access to SME functionality trapped";
		case 39U:
			return "(FEAT_MOPS) Memory Operation Exception";
		case 45U:
			return "(FEAT_GCS) GCS exception";
		case 61U:
			return "(FEAT_EBEP) PMU exception";
#endif
		default:
			return "Reserved";
	}
}


void hal_exceptionsDumpContext(char *buff, exc_context_t *ctx, unsigned int n)
{
	size_t i = 0;
	u8 j;
	const char *toAdd;

	toAdd = "\nException #";
	(void)hal_strcpy(&buff[i], toAdd);
	i += hal_strlen(toAdd);
	buff[i++] = '0' + n / 10U;
	buff[i++] = '0' + n % 10U;
	buff[i++] = ':';
	buff[i++] = ' ';
	toAdd = exceptionClassStr(n);
	(void)hal_strcpy(&buff[i], toAdd);
	i += hal_strlen(toAdd);

	char prefix[6] = "    =";
	for (j = 0; j < 29U; j++) {
		prefix[0] = ((j % 4U) == 0U) ? '\n' : ' ';
		if (j < 10U) {
			prefix[1] = ' ';
			prefix[2] = 'x';
		}
		else {
			prefix[1] = 'x';
			prefix[2] = (char)('0' + (j / 10U));
		}

		prefix[3] = (char)('0' + (j % 10U));
		i += hal_i2s(prefix, &buff[i], ctx->cpuCtx.x[j], 16U, 1U);
	}

	i += hal_i2s("  fp=", &buff[i], ctx->cpuCtx.x[29], 16U, 1U);
	i += hal_i2s("  lr=", &buff[i], ctx->cpuCtx.x[30], 16U, 1U);
	i += hal_i2s("  sp=", &buff[i], ctx->cpuCtx.sp, 16U, 1U);

	i += hal_i2s("\npsr=", &buff[i], ctx->cpuCtx.psr, 16U, 1U);
	i += hal_i2s("  pc=", &buff[i], ctx->cpuCtx.pc, 16U, 1U);
	i += hal_i2s(" esr=", &buff[i], ctx->esr, 16U, 1U);
	i += hal_i2s(" far=", &buff[i], ctx->far, 16U, 1U);

	buff[i++] = '\n';
	buff[i] = '\0';
}


/* Print a call-stack backtrace for a kernel exception. Resolve the printed
 * addresses offline with `aarch64-phoenix-addr2line -e phoenix-<target>.elf <addr>`.
 *
 * Deliberately a SEPARATE step from hal_exceptionsDumpContext, called by the
 * kernel fault handlers only AFTER the register dump has already been printed:
 * this walk dereferences the faulting stack, and on a stack-overflow /
 * stack-corruption crash a frame pointer can point into an unmapped page, so
 * the walk itself may take a nested fault. Ordering it after the register print
 * means such a nested fault costs only the backtrace, never the registers.
 *
 * pc and lr are printed first. lr matters because a crash in a LEAF function has
 * no stack frame of its own, so its caller lives only in lr, not on the stack;
 * walking x29 alone would silently skip it. (For a NON-leaf fault lr is stale -
 * it points into the faulting function's last callee return - hence it is
 * labelled, not presented as a true frame.) The remaining lines walk the AAPCS64
 * frame-pointer chain (the kernel is built -fno-omit-frame-pointer): x29 points
 * at [saved x29, saved x30], i.e. [fp]=caller fp, [fp+8]=return address.
 *
 * The walk only ever dereferences fp values that are 16-byte aligned, ascend
 * monotonically, and lie inside a 16 KiB window anchored on the first frame
 * pointer (itself validated as within 64 KiB of sp), and is depth-capped. That
 * rejects a corrupt fp by the bound rather than faulting - but note it cannot
 * prove the pages are mapped, which is the residual reason this runs last. */
static void hal_exceptionsBacktrace(exc_context_t *ctx)
{
	char buff[512];
	size_t i = 0;
	const unsigned long sp = ctx->cpuCtx.sp;
	unsigned long fp = ctx->cpuCtx.x[29];
	unsigned long spdiff = (fp > sp) ? (fp - sp) : (sp - fp);
	unsigned long lo = fp & ~0xfUL;
	unsigned long hi = lo + 0x4000UL; /* walk at most 16 KiB up toward the stack base */
	unsigned int depth;

	(void)hal_strcpy(&buff[i], "backtrace:");
	i += hal_strlen("backtrace:");
	i += hal_i2s("\n  pc=", &buff[i], ctx->cpuCtx.pc, 16U, 1U);
	i += hal_i2s("\n  lr=", &buff[i], ctx->cpuCtx.x[30], 16U, 1U);

	if ((fp != 0UL) && ((fp & 0xfUL) == 0UL) && (spdiff < 0x10000UL)) {
		for (depth = 0U; depth < 16U; depth++) {
			unsigned long nextfp, ret;
			if ((fp < lo) || (fp >= hi) || ((fp & 0xfUL) != 0UL)) {
				break;
			}
			nextfp = *(volatile unsigned long *)(addr_t)fp;
			ret = *(volatile unsigned long *)(addr_t)(fp + 8UL);
			i += hal_i2s("\n  ", &buff[i], ret, 16U, 1U);
			if (nextfp <= fp) {
				break; /* must ascend the stack */
			}
			fp = nextfp;
		}
	}

	buff[i++] = '\n';
	buff[i] = '\0';
	hal_consolePrint(ATTR_BOLD, buff);
}


static void exceptions_defaultHandler(unsigned int n, exc_context_t *ctx)
{
	char buff[SIZE_CTXDUMP];

	hal_exceptionsDumpContext(buff, ctx, n);
	hal_consolePrint(ATTR_BOLD, buff);
	hal_exceptionsBacktrace(ctx);

#ifdef NDEBUG
	hal_cpuReboot();
#else
	for (;;) {
		hal_cpuHalt();
	}
#endif
}


/* TD-10: dedicated SError policy.
 *
 * SError is an asynchronous abort: ELR_EL1/PC only approximate where
 * execution was when it was taken, and FAR_EL1 is not meaningful. The
 * decisive information is in ESR_EL1 (EA, AET, DFSC, IDS), which the
 * context dump prints raw.
 *
 * Policy: ALWAYS halt with the dump on the console - never reboot, even
 * in release builds where exceptions_defaultHandler would reboot. On the
 * Pi 4 an SError signals a genuine, otherwise-invisible fault (external
 * bus / parity abort) that was previously hidden by masking SError across
 * all paths; a silent reset would destroy that evidence. Revisit a
 * contained-error continue policy only once the real SError sources on
 * this SoC have been characterized. */
static void exceptions_serrorHandler(unsigned int n, exc_context_t *ctx)
{
	char buff[SIZE_CTXDUMP];

	hal_exceptionsDumpContext(buff, ctx, n);
	hal_consolePrint(ATTR_BOLD, "\nasynchronous SError taken - halting (decode ESR.{EA,AET,DFSC,IDS}):\n");
	hal_consolePrint(ATTR_BOLD, buff);
	hal_exceptionsBacktrace(ctx);

	for (;;) {
		hal_cpuHalt();
	}
}


/* Value-trap window for the watchpoint handler, set by hal_cpuWatchpointSet
 * (cpu.c). trapHi == 0 => halt on any store. */
extern addr_t hal_wpTrapLo;
extern addr_t hal_wpTrapHi;


/* Self-hosted A72 watchpoint (Route A, debug/diagnostic). When a watchpoint
 * armed via platformctl(pctl_watchpoint) fires:
 *  - halt+dump (pc/ELR = writer, far = watched addr) if either the trap window
 *    is disabled (trapHi == 0), the faulting store is not the simple
 *    `str Xt, [Xn{,#imm}]` form we can emulate, or the value being stored is in
 *    [trapLo, trapHi) (e.g. a code-pointer wild write — the thing we're hunting).
 *    Never reboots, even under NDEBUG, so the culprit is readable over UART; the
 *    preemptive scheduler keeps the rest of the system alive.
 *  - otherwise the store is legitimate (NULL / heap pointer): emulate it (the
 *    watchpoint fires before the access completes) and step past it, leaving the
 *    watchpoint armed so a later wild write is still caught.
 * A72 is ARMv8.0 (no FEAT_PAN), so EL1 may read the faulting EL0 instruction and
 * write the EL0 store target directly. */
static void exceptions_watchpointHandler(unsigned int n, exc_context_t *ctx)
{
	unsigned int instr = *(volatile unsigned int *)(addr_t)ctx->cpuCtx.pc;
	unsigned int rt = instr & 0x1fU;
	/* STR (64-bit, unsigned offset): `str Xt, [Xn{,#imm}]` — the only form
	 * list.c emits for `*list = X`. */
	int isStrImm = ((instr & 0xffc00000U) == 0xf9000000U) ? 1 : 0;
	unsigned long value = (rt == 31U) ? 0UL : ctx->cpuCtx.x[rt];
	int trap;

	if (hal_wpTrapHi == 0UL) {
		trap = 1;
	}
	else {
		trap = ((isStrImm == 0) || ((value >= (unsigned long)hal_wpTrapLo) && (value < (unsigned long)hal_wpTrapHi))) ? 1 : 0;
	}

	if (trap != 0) {
		char buff[SIZE_CTXDUMP];
		hal_exceptionsDumpContext(buff, ctx, n);
		hal_consolePrint(ATTR_BOLD, "\nwatchpoint hit - halting (pc=writer, far=watched addr):\n");
		hal_consolePrint(ATTR_BOLD, buff);
		hal_exceptionsBacktrace(ctx);
		for (;;) {
			hal_cpuHalt();
		}
	}

	/* Legitimate store: far is the exact accessed address; emulate and step. */
	*(volatile unsigned long *)(addr_t)ctx->far = value;
	ctx->cpuCtx.pc += 4UL;
}


void threads_setupUserReturn(void *retval, cpu_context_t *ctx);


/* parasoft-suppress-next-line MISRAC2012-RULE_8_4 "Usage in assembly" */
void exceptions_dispatch(unsigned int n, exc_context_t *ctx)
{
	if (n >= N_EXCEPTIONS) {
		return;
	}

	exceptions.handler[n](n, ctx);
	/* Handle signals if necessary */
	if (hal_cpuSupervisorMode(&ctx->cpuCtx) == 0) {
		threads_setupUserReturn((void *)ctx->cpuCtx.x[0], &ctx->cpuCtx);
	}
}


vm_prot_t hal_exceptionsFaultType(unsigned int n, exc_context_t *ctx)
{
	vm_prot_t prot = 0;
	u32 iss;

	switch (n) {
#ifdef __TARGET_AARCH64A53
		case EXC_SERROR:
			/* Some SError exceptions can result from writing to an invalid address */
			iss = (u32)(ctx->esr & ((1UL << 25) - 1U));
			if ((iss & (1UL << 24)) == 0U) {
				return PROT_NONE;
			}

			iss = (iss & 0x3U) | ((iss >> 20) & 0xcU);
			prot |= (iss == 2U) ? PROT_WRITE : 0U; /* SLVERR */
			prot |= (iss == 0U) ? PROT_WRITE : 0U; /* DECERR */
			return prot;
#endif
		/* parasoft-suppress-next-line MISRAC2012-RULE_16_1 MISRAC2012-RULE_16_3 "Intentional fall-through" */
		case EXC_INSTR_ABORT_EL0:
			prot |= PROT_USER;
			/* Fall-through */

		case EXC_INSTR_ABORT_EL1:
			prot |= PROT_EXEC | PROT_READ;
			return prot;

		/* parasoft-suppress-next-line MISRAC2012-RULE_16_1 MISRAC2012-RULE_16_3 "Intentional fall-through" */
		case EXC_DATA_ABORT_EL0:
			prot |= PROT_USER;
			/* Fall-through */

		case EXC_DATA_ABORT_EL1:
			iss = (u32)(ctx->esr & ((1UL << 25) - 1U));
			prot |= ((iss & (1UL << 6)) == 0U) ? PROT_READ : PROT_WRITE;
			return prot;

		default:
			return PROT_NONE;
	}
}

ptr_t hal_exceptionsPC(exc_context_t *ctx)
{
	return ctx->cpuCtx.pc;
}


void *hal_exceptionsFaultAddr(unsigned int n, exc_context_t *ctx)
{
	unsigned long iss;

	switch (n) {
		/* parasoft-suppress-next-line MISRAC2012-RULE_16_1 MISRAC2012-RULE_16_3 "Intentional fall-through" */
		case EXC_INSTR_ABORT_EL0: /* Fall-through */
		/* parasoft-suppress-next-line MISRAC2012-RULE_16_1 MISRAC2012-RULE_16_3 "Intentional fall-through" */
		case EXC_INSTR_ABORT_EL1: /* Fall-through */
		/* parasoft-suppress-next-line MISRAC2012-RULE_16_1 MISRAC2012-RULE_16_3 "Intentional fall-through" */
		case EXC_DATA_ABORT_EL0: /* Fall-through */
		case EXC_DATA_ABORT_EL1:
			iss = ctx->esr & ((1UL << 25) - 1U);
			return ((iss & (1UL << 10)) == 0U) ? (void *)ctx->far : NULL;

		default:
			return NULL;
	}
}


int hal_exceptionsSetHandler(unsigned int n, excHandlerFn_t handler)
{
	int ret = 0;
	spinlock_ctx_t sc;

	hal_spinlockSet(&exceptions.lock, &sc);
	if (n < N_EXCEPTIONS) {
		exceptions.handler[n] = handler;
	}
	else if (n == EXC_DEFAULT) {
		exceptions.defaultHandler = handler;
	}
	else if (n == EXC_PAGEFAULT) {
		exceptions.handler[EXC_INSTR_ABORT_EL0] = handler;
		exceptions.handler[EXC_INSTR_ABORT_EL1] = handler;
		exceptions.handler[EXC_DATA_ABORT_EL0] = handler;
		exceptions.handler[EXC_DATA_ABORT_EL1] = handler;
	}
	else {
		ret = -1;
	}

	hal_spinlockClear(&exceptions.lock, &sc);

	return ret;
}


void _hal_exceptionsInit(void)
{
	unsigned int i;
	hal_spinlockCreate(&exceptions.lock, "exceptions.lock");

	exceptions.defaultHandler = exceptions_defaultHandler;
	for (i = 0; i < N_EXCEPTIONS; i++) {
		exceptions.handler[i] = exceptions_trampoline;
	}

	/* TD-10: SError gets a dedicated dump-and-halt handler (see above). */
	exceptions.handler[EXC_SERROR] = exceptions_serrorHandler;

	/* Route A: EL0 data watchpoints (armed via platformctl(pctl_watchpoint))
	 * dump-and-halt instead of the default reboot, so the writer is readable. */
	exceptions.handler[EXC_WATCHPOINT_EL0] = exceptions_watchpointHandler;
}
