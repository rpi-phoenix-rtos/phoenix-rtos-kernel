/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Machine context delivered to SA_SIGINFO signal handlers (aarch64)
 *
 * Copyright 2026 Phoenix Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_ARCH_AARCH64_UCONTEXT_H_
#define _PH_ARCH_AARCH64_UCONTEXT_H_


#define _PH_HAVE_MCONTEXT


/* Field names and order follow Linux's `struct sigcontext`, so code written for
 * Linux/aarch64 (uc_mcontext.pc, .sp, .regs[29], .fault_address) builds
 * unchanged. Linux's 4 KiB __reserved area (FP/SIMD and extension records) is
 * not provided: FP/SIMD state is saved and restored across the handler, but it
 * is not exposed here. */
typedef struct {
	unsigned long long fault_address;
	unsigned long long regs[31];
	unsigned long long sp;
	unsigned long long pc;
	unsigned long long pstate;
} __attribute__((aligned(16))) mcontext_t;


#endif
