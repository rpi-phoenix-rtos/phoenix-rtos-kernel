/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Library routines
 *
 * Copyright 2012, 2014, 2016 Phoenix Systems
 * Copyright 2001, 2006 Pawel Pisarczyk
 * Author: Pawel Pisarczyk, Pawel Kolodziej
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_LIB_LIB_H_
#define _PH_LIB_LIB_H_

#include "hal/hal.h"
#include "printf.h"
#include "bsearch.h"
#include "rand.h"
#include "rb.h"
#include "list.h"
#include "assert.h"
#include "strutil.h"
#include "idtree.h"


/* TODO: migrate to C11 stdatomic - check the performance impact of seq_cnt (mandatory by MISRA) */
/* casts needed as return type is not always the same as *ptr (why? GCC bug?) */
#if defined(__aarch64__) && (NUM_CPUS == 1)

/* Single-core scope (TD-01/TD-11): the shipping Pi 4 config schedules on
 * cpu0 only (NUM_CPUS == 1). In that build the AArch64 exclusive-access
 * atomics GCC emits for the __atomic_* builtins are unnecessary, so mask
 * interrupts and use plain memory updates, matching the single-core
 * AArch64 spinlock path. SMP builds (NUM_CPUS != 1) use the real
 * __atomic_* path below. Full Cortex-A72 SMP scheduling is future work. */
#define lib_atomicIncrement(ptr) ({ \
	spinlock_ctx_t _sc; \
	typeof(*(ptr)) _ret; \
	hal_spinlockSet(NULL, &_sc); \
	_ret = ++(*(ptr)); \
	hal_spinlockClear(NULL, &_sc); \
	_ret; \
})


#define lib_atomicDecrement(ptr) ({ \
	spinlock_ctx_t _sc; \
	typeof(*(ptr)) _ret; \
	hal_spinlockSet(NULL, &_sc); \
	_ret = --(*(ptr)); \
	hal_spinlockClear(NULL, &_sc); \
	_ret; \
})

#else

#define lib_atomicIncrement(ptr) ((typeof(*(ptr)))__atomic_add_fetch((ptr), 1, __ATOMIC_RELAXED))


#define lib_atomicDecrement(ptr) ((typeof(*(ptr)))__atomic_sub_fetch((ptr), 1, __ATOMIC_ACQ_REL))

#endif


#define max(a, b) ({ \
	__typeof__(a) _a = (a); \
	__typeof__(b) _b = (b); \
	_a > _b ? _a : _b; \
})


#define min(a, b) ({ \
	__typeof__(a) _a = (a); \
	__typeof__(b) _b = (b); \
	_a > _b ? _b : _a; \
})


#include "cbuffer.h"


#define swap(a, b) ({ \
	__typeof__(a) tmp = (a); \
	(a) = (b); \
	(b) = (tmp); \
})


#define round_page(x) (((x) + SIZE_PAGE - 1U) & ~(SIZE_PAGE - 1U))

/* parasoft-suppress-next-line MISRAC2012-RULE_20_7-a "__builtin_offsetof is built-in function and handles it arguments safely" */
#define offsetof(st, m) __builtin_offsetof(st, m)


#endif
