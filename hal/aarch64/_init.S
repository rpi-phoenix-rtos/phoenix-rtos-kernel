/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Low-level initialization for AArch64 architecture
 *
 * Copyright 2024 Phoenix Systems
 * Author: Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define __ASSEMBLY__

#include "config.h"
#include <arch/cpu.h>
#include <arch/pmap.h>

.extern pmap_common
.extern stacks_el1
.extern hal_syspage
.extern relOffs
.extern nCpusStarted
.extern _end


#define UARTFR_OFFSET 0x18
#define UARTIBRD_OFFSET 0x24
#define UARTFBRD_OFFSET 0x28
#define UARTLCRH_OFFSET 0x2c
#define UARTCR_OFFSET 0x30
#define UARTFR_TXFF   (1 << 5)
#define EARLY_UART_DEVICE_DESCR 0x6000000000070b
#define EARLY_UART_DEVICE_BLOCK 0x60000000000709

.macro uart_putc imm
991:
	ldr x3, =(PL011_TTY_BASE + UARTFR_OFFSET)
	ldr w4, [x3]
	tst w4, #UARTFR_TXFF
	b.ne 991b
	ldr x3, =PL011_TTY_BASE
	mov w4, #\imm
	str w4, [x3]
.endm


#if defined(PL011_TTY_EARLY_VADDR)
.macro uart_putc_virt imm
992:
	ldr x3, =(PL011_TTY_EARLY_VADDR + UARTFR_OFFSET)
	ldr w4, [x3]
	tst w4, #UARTFR_TXFF
	b.ne 992b
	ldr x3, =PL011_TTY_EARLY_VADDR
	mov w4, #\imm
	str w4, [x3]
.endm
#else
.macro uart_putc_virt imm
.endm
#endif


.macro uart_tag2 c1 c2
	uart_putc 10
	uart_putc \c1
	uart_putc \c2
	uart_putc 10
.endm

.macro early_vector slot
        mov x18, #\slot
        b _early_exception_common
.rept 30
        nop
.endr
.endm


#if defined(PL011_TTY_CLOCK) && (PL011_TTY_CLOCK == 48000000u)
.macro uart_reinit_115200
	ldr x3, =PL011_TTY_BASE
	mov w4, wzr
	str w4, [x3, #UARTCR_OFFSET]
	mov w4, #26
	str w4, [x3, #UARTIBRD_OFFSET]
	mov w4, #3
	str w4, [x3, #UARTFBRD_OFFSET]
	mov w4, #0x70
	str w4, [x3, #UARTLCRH_OFFSET]
	mov w4, #0x301
	str w4, [x3, #UARTCR_OFFSET]
	dsb sy
	isb
.endm
#else
.macro uart_reinit_115200
.endm
#endif


/* 16-bit ASID
 * Params for TTBR1:
 * * 4 KB granule
 * * Inner shareable
 * * Outer cacheable
 * * Inner cacheable
 * * Disable translation through TTBR1
 * * ASID determined by TTBR0
 * * 1 GB virtual address range
 * Params for TTBR0:
 * * 4 KB granule
 * * Inner shareable
 * * Outer cacheable
 * * Inner cacheable
 * * Enable translation through TTBR0
 * * 512 GB virtual address range */
#define TCR_EL1_VALUE ( \
        (1 << 36) | \
        (2 << 30) | \
        (3 << 28) | \
        (1 << 26) | \
        (1 << 24) | \
        (1 << 23) | \
        (0 << 22) | \
        (34 << 16) | \
        (0 << 14) | \
        (3 << 12) | \
        (1 << 10) | \
        (1 << 8) | \
        (0 << 7) | \
        (25 << 0) \
)
/* Index 0: Normal memory, inner and outer cacheable, write-back, non-transient read-allocate, write-allocate
 * Index 1: Normal memory, inner and outer non-cacheable
 * Index 2: Device memory, Gathering, re-ordering disabled, early write acknowledge enabled (equiv. to AArch32 "Device" memory)
 * Index 3: Device memory, Gathering, re-ordering, early write acknowledge all disabled (equiv. to AArch32 "Strongly ordered" memory)
*/
#define MAIR_EL1_VALUE 0x444FF

/* Privileged RWX, unprivileged no-access, normal cacheable, inner shareable, access flag, global */
#define DEFAULT_ATTRS 0x703

/* Needs to be updated if hal_syspage_t is changed */
#if defined(HAS_GRAPHICS) && (HAS_GRAPHICS != 0)
#define SYSPAGE_SIZE_OFFSET 40
#define SYSPAGE_PKERNEL_OFFSET 48
#else
#define SYSPAGE_SIZE_OFFSET 24
#define SYSPAGE_PKERNEL_OFFSET 32
#endif

#define PMAP_COMMON_KERNEL_TTL2 (pmap_common)
#define PMAP_COMMON_KERNEL_TTL3 (pmap_common + SIZE_PAGE)
#define PMAP_COMMON_DEVICES_TTL3 (pmap_common + (2 * SIZE_PAGE))
#define PMAP_COMMON_SCRATCH_TT (pmap_common + (3 * SIZE_PAGE))
#define PMAP_COMMON_SCRATCH_PAGE (pmap_common + (4 * SIZE_PAGE))
#define PMAP_COMMON_STACK (pmap_common + (5 * SIZE_PAGE))
#define VADDR_SYSPAGE (_hal_syspageCopied)

.section .init, "ax"
.global _start
.type _start, %function

.org 0
_start:
	uart_reinit_115200
        /* DEBUG: Check current exception level */
        mrs x0, CurrentEL
        uart_putc 90 /* DEBUG: CurrentEL value */
        
        /* Assumptions:
         * x9 => PA of syspage from PLO */
        uart_putc 75
	/* Mask all interrupts */
	msr daifSet, #0xf

	/* Interesting bits:
		EE == 0, E0E == 0 => little-endian at EL1 and EL0
		WXN == 0 => write permission doesn't imply execute never
		nTWE == 0, nTWI == 0 => trap WFE/WFI instructions at EL0
		UMA == 0 => trap accesses to DAIF system reg. from EL0
		SED == 1 => trap SETEND instruction in AArch32
		SA0 == 1, SA == 1 => alignment checks for SP accesses
		A == 0 => disable data alignment checks
		C, I, M == 0 => disable D-cache, I-cache, MMU (for now)
		UCT == 1 => don't trap accesses to CTR_EL0 (cache type reg.) from EL0
		DZE == 1 => don't trap DC ZVA instructions from EL0
		UCI == 0 => trap cache maintenance instructions executed at EL0
	 */
	ldr x0, =0x30c0c938
	msr sctlr_el1, x0
	
	/* DEBUG: Read back SCTLR_EL1 to verify */
	mrs x1, sctlr_el1
	uart_putc 91 /* DEBUG: SCTLR_EL1 after write */

	/* Enable FPU/SIMD access (like Circle OS and bare metal examples) */
	/* BIT(20) + BIT(21) = FPEN (don't trap FP/SIMD) */
	/* BIT(16) + BIT(17) = ZEN (don't trap SVE) */
	mov x0, #((0x3 << 20) | (0x3 << 16))
	msr cpacr_el1, x0
	isb

	/* Invalidate caches and TLB - simplified approach */
	ic ialluis /* Invalidate instruction cache */
	tlbi vmalle1 /* Invalidate all TLB entries */
	dsb ish
	isb

	/* Enable performance monitors */
	mrs x0, pmcr_el0
	orr x0, x0, #0x7 /* reset cycle counter, reset event counter, enable */
	orr x0, x0, #0x40 /* activate 64-bit counting mode */
	msr pmcr_el0, x0
	mrs x0, pmcntenset_el0
	orr x0, x0, #(1 << 31)
	msr pmcntenset_el0, x0

	uart_putc 76

#if defined(__TARGET_AARCH64A53) || defined(__TARGET_AARCH64A72)
        /* Enable SMP for A53 or A72 */
        /* Temporarily disabled to test if this is causing the hang */
        uart_putc 83 /* DEBUG: Before SMP enable */
        /* mov x0, #(1 << 6) */ /* CPUECTLR_EL1_SMPEN only */
        /* msr s3_1_c15_c2_1, x0 */
        /* isb */ /* Ensure write completes */
        uart_putc 84 /* DEBUG: After SMP enable */
#endif

/* Temporarily disable A53-specific CPUACTLR_EL1 setup to test if it's causing hang on A72 */
/* #ifdef __TARGET_AARCH64A53 */
        /* L1 Data prefetch control - DISABLED FOR A72 TESTING */

	/* Set ENDCCASCI bit in CPUACTLR_EL1 register - DISABLED FOR A72 TESTING */
	/* ldr	x0, =0x1000080ca000 */
	/* msr	s3_1_c15_c2_0, x0 */ /* CPUACTLR_EL1 */
/* #endif */

	uart_putc 85 /* DEBUG: Before M marker */
	uart_putc 77
	uart_putc 86 /* DEBUG: After M marker */

	/* Set up MMU */
	uart_tag2 88, 49
	ldr x0, =(TCR_EL1_VALUE)
	mrs x1, id_aa64mmfr0_el1
	and x1, x1, #0x7
	orr x0, x0, x1, lsl #32
	msr tcr_el1, x0

	ldr x0, =(MAIR_EL1_VALUE)
	msr mair_el1, x0

	/* Note: ADRP instruction will generate a PC-relative address.
	 * Because now PC is in physical memory, the resulting address will also be physical */
	/* Create temporary TTL1 that maps 2 GB around syspage->pkernel */
	adrp x0, PMAP_COMMON_SCRATCH_TT
	bl _fill_page_zero
	/* Map syspage->pkernel ~ (syspage->pkernel + 1 GB) VA => syspage->pkernel ~ (syspage->pkernel + 1 GB) PA */

	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]
	mov x3, #0xf01 /* Privileged RWX, unprivileged no-access, normal cacheable, inner shareable, access flag, not global */
	mov x4, #(1 << 30)
	lsr x2, x1, #30
	orr x1, x3, x2, lsl #30
	str x1, [x0, x2, lsl 3]
	add x1, x1, x4
	add x2, x2, #1
	str x1, [x0, x2, lsl 3]

	/* Map syspage VA => syspage PA */
	lsr x2, x9, #30
	orr x1, x3, x2, lsl #30
	str x1, [x0, x2, lsl 3]

	/* Map the 1 GB block containing PL011 so an early post-MMU exception
	 * handler can still print through the physical UART path before the normal
	 * kernel VBAR and full page-table setup are live. */
	ldr x1, =PL011_TTY_BASE
	lsr x2, x1, #30
	ldr x3, =EARLY_UART_DEVICE_BLOCK
	orr x1, x3, x2, lsl #30
	str x1, [x0, x2, lsl 3]
	msr ttbr0_el1, x0
	uart_tag2 88, 50

	adr x0, _early_vector_table
	msr vbar_el1, x0

	adrp x0, PMAP_COMMON_KERNEL_TTL2
	msr ttbr1_el1, x0


	/* Linux arm64 explicitly invalidates page tables populated with the MMU
	 * disabled to discard any speculatively loaded cache lines before the
	 * page-table walker starts using them. Do the same for the contiguous
	 * early kernel MMU region:
	 *   PMAP_COMMON_KERNEL_TTL2
	 *   PMAP_COMMON_KERNEL_TTL3
	 *   PMAP_COMMON_DEVICES_TTL3
	 *   PMAP_COMMON_SCRATCH_TT
	 *   PMAP_COMMON_SCRATCH_PAGE
	 *
	 * However, on Cortex-A72, cache maintenance with MMU disabled can cause hangs.
	 * Skip this invalidation for now and rely on the post-MMU-enable invalidation.
	 */
	/* adrp x0, PMAP_COMMON_KERNEL_TTL2 */
	/* adrp x1, PMAP_COMMON_STACK */
	/* bl _inval_dcache_range */

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Turn on MMU and caches */

	uart_tag2 88, 51
	dsb ish
	/* Enable MMU first, then caches separately to avoid potential issues */
	mrs x0, sctlr_el1
	orr x0, x0, #(1 << 0)   /* Enable MMU only first */
	msr sctlr_el1, x0
	isb
	/* Match Linux's arm64 early-MMU sequence: discard any instructions
	 * speculatively fetched before the new translation regime became active. */
	ic iallu
	dsb nsh
	isb
	
	/* Now enable caches after MMU is stable */
	mrs x0, sctlr_el1
	orr x0, x0, #(1 << 12)  /* Enable I-cache */
	orr x0, x0, #(1 << 2)   /* Enable D-cache */
	msr sctlr_el1, x0
	isb
	/* Translation is now taking place through PMAP_COMMON_SCRATCH_TT
	 * Before we can jump to kernel in virtual memory we must set up PMAP_COMMON_KERNEL_TTL2 table */

	/* Set up kernel stack NOW that MMU is enabled and virtual memory is working */
	adrp x0, PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1

	/* Zero out an additional page that will be used a bit later */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	bl _fill_page_zero

	/* Initialize counter of started CPUs.
	 * The counter starts out uninitialized. We want to initialize it only once by only one core -
	 * but cores can start in any order and the variable must be initialized before any core starts waiting.
	 * To solve this, we use a flag in the code section. Right now the code section is writable,
	 * but it will be changed to read-only later. */
	adrp x1, nCpusStarted
	adr x2, _initNCpusFlag
.LretryWrite:
	ldaxr w0, [x2]
	cbz w0, .LnCpusAlreadyInited
	stlxr w3, wzr, [x2]
	cbnz w3, .LretryWrite
	str wzr, [x1, #:lo12:nCpusStarted]
.LnCpusAlreadyInited:
	clrex

	/* Trap all cores except core 0 */
	mrs x8, mpidr_el1
	and x8, x8, #0xff
	cbnz x8, _other_core_trap

	/* Set up translation tables */
	adrp x0, PMAP_COMMON_KERNEL_TTL2
	bl _fill_page_zero

	/* Map TTL2 VADDR_KERNEL ~ (VADDR_KERNEL + 2 MB) => PMAP_COMMON_KERNEL_TTL3 */
	adrp x1, PMAP_COMMON_KERNEL_TTL3
	orr x1, x1, #0x3 /* Valid, table descriptor */
	str x1, [x0]

	/* Map TTL2 (VADDR_MAX - 2 MB) ~ VADDR_MAX => PMAP_COMMON_DEVICES_TTL3 */
	add x2, x0, #(SIZE_PAGE - 8)
	adrp x0, PMAP_COMMON_DEVICES_TTL3
	orr x1, x0, #0x3 /* Valid, table descriptor */
	str x1, [x2]

	/* Map TTL3 (VADDR_MAX - 2 MB) ~ VADDR_MAX => no access */
	bl _fill_page_zero

#if defined(PL011_TTY_EARLY_VADDR)
	/* Reserve the first device TTL3 entry for a temporary post-MMU PL011 probe. */
	ldr x1, =(PL011_TTY_BASE | EARLY_UART_DEVICE_DESCR)
	str x1, [x0]
#endif

	/* Map TTL3 VADDR_KERNEL ~ (VADDR_KERNEL + 2 MB) => syspage->pkernel ~ (syspage->pkernel + 2 MB) PA */
	adrp x0, PMAP_COMMON_KERNEL_TTL3
	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]
	mov x2, #(DEFAULT_ATTRS)
	orr x1, x1, x2
	bl _fill_page_descr

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Activate translation through ttbr1_el1 */
	mrs x0, tcr_el1
	bic x0, x0, #(1 << 23)
	msr tcr_el1, x0
	isb

	uart_putc_virt 78

	/* DEBUG: Marker before former branch point */
	mov w0, #89  /* ASCII 'Y' - about to enter virtual memory code */
	bl uart_putc_reg

	/* INLINE the critical _core_0_virtual code to avoid branch issues */
	uart_putc_virt 79  /* O marker - entered virtual memory code */
	
	/* Compute relOffs and hal_syspage using the ABSOLUTE linked high VA
	 * of _hal_syspageCopied (via literal pool), not the PC-relative form.
	 *
	 * Primary-core execution never transitions PC to the high canonical VA:
	 * on Cortex-A72 + this TCR/MAIR/pmap configuration, data reads through
	 * TTBR1 work, but instruction fetch through TTBR1 fails (verified by
	 * a probe that successfully read kernel[0] at 0xffffffffc0000000 but
	 * faulted on `br x0` to the same region). So PC stays at the low
	 * physical load address and every `adrp` here resolves to a low PA.
	 * If we used adrp for VADDR_SYSPAGE, relOffs would be ~0 and
	 * hal_syspageRelocate would be a no-op — exactly the bug that made
	 * syspage_init walk raw plo PAs in syspage.c.
	 *
	 * `ldr x1, =VADDR_SYSPAGE` loads the value 0xffffffffc00xxxxx from the
	 * literal pool, giving the correct linked VA independent of PC. The
	 * resulting relOffs = VA_dest - PA_src makes
	 * hal_syspageRelocate(src_PA) = src_PA + relOffs = corresponding
	 * high-VA slot inside _hal_syspageCopied, which TTBR1 data reads
	 * can then dereference. */
	ldr x1, =VADDR_SYSPAGE          /* absolute linked VA of dest (high) */
	sub x2, x1, x9                  /* relOffs = VA_dest - PA_src */
	adrp x0, relOffs
	str x2, [x0, #:lo12:relOffs]
	adrp x0, hal_syspage
	str x1, [x0, #:lo12:hal_syspage]

	/* Clean+invalidate the plo-owned syspage cache range before reading it.
	 * plo builds the syspage with MMU+D-cache enabled; when it jumps to the
	 * kernel it does NOT flush D-cache. Some dirty lines (seen as bit-level
	 * nondeterminism in the later half of the copied bytes) therefore never
	 * reach DDR before this kernel reads it via identity-mapped PA. Force a
	 * clean+invalidate by VA over [x9, x9+size) so DDR is authoritative. */
	ldr x0, [x9, #(SYSPAGE_SIZE_OFFSET)]
	add x1, x9, x0
	mov x0, x9
	bl _clean_inval_dcache_range

	/* The copy itself uses a LOW-PA destination (via adrp), because
	 * instruction-side TTBR1 fetch is broken and we want to avoid
	 * relying on TTBR1 data writes for bulk bytes until they are
	 * explicitly confirmed to work. The low PA of _hal_syspageCopied
	 * is identity-mapped through TTBR0 by the scratch TT. */
	adrp x1, VADDR_SYSPAGE
	add x1, x1, :lo12:VADDR_SYSPAGE
	ldr x0, [x9, #(SYSPAGE_SIZE_OFFSET)]
1:
	cmp x0, #8
	b.lo 2f
	ldr x3, [x9], #8
	str x3, [x1], #8
	sub x0, x0, #8
	cbnz x0, 1b
	b 3f
2:
	cbz x0, 3f
	ldrb w3, [x9], #1
	strb w3, [x1], #1
	sub x0, x0, #1
	cbnz x0, 2b
3:
	uart_putc_virt 80  /* P marker - syspage copy complete */

	/* INLINE the critical parts of _set_up_vbar_and_stacks to avoid function call issues */
	/* Set vector table */
	adrp x0, _vector_table
	add x0, x0, :lo12:_vector_table
	msr vbar_el1, x0
	
	/* Debug: After setting vector table (S marker) */
	uart_putc_virt 83

	/* Set TTBR0 translation table */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	
	/* Strong synchronization */
	isb
	dsb sy
	
	/* Debug: After TTBR0 setup (T marker) */
	uart_putc_virt 84

	/* Set up kernel stack - AFTER MMU is enabled */
	msr spsel, #1
	adrp x0, PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1
	
	/* Debug: After stack setup (U marker) */
	uart_putc_virt 85
	
	/* Final assembly debug marker before branching to C */
	uart_putc_virt 90  /* ASCII 'Z' - about to enter main() */
	
	/* Final debug marker before branching to C */
	uart_putc_virt 98  /* ASCII 'b' - about to branch to main() */

	/* Branch to main - this is the actual kernel entry point */
	b main


_other_core_trap:
	dsb ish
	wfe
	ldr x0, [x1, #:lo12:nCpusStarted]
	cbz x0, _other_core_trap

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Activate translation through ttbr1_el1 */
	mrs x0, tcr_el1
	bic x0, x0, #(1 << 23)
	msr tcr_el1, x0
	isb

	ldr x0, =_other_core_virtual
	br x0

_other_core_virtual:
	bl _set_up_vbar_and_stacks
	bl _hal_interruptsInitPerCPU
	bl _hal_cpuInit

	msr daifClr, #7
1:
	wfi
	b 1b


_fill_page_zero:
	/* Fill page with zeroes
	 * x0 => base address
	 * clobbers x1 */
	add x1, x0, #(SIZE_PAGE)
1:
	stp xzr, xzr, [x1, #-0x10]!
	stp xzr, xzr, [x1, #-0x10]!
	cmp x0, x1
	b.lo 1b
	ret

_fill_page_descr:
	/* Fill page with descriptor entries
	 * x0 => base address
	 * x1 => target value
	 * clobbers x0, x1, x2, x3 */
	add x2, x1, #(SIZE_PAGE)
	add x3, x0, #(SIZE_PAGE)
1:
	stp x1, x2, [x0], #0x10
	add x1, x1, #(2 * SIZE_PAGE)
	add x2, x2, #(2 * SIZE_PAGE)
	stp x1, x2, [x0], #0x10
	add x1, x1, #(2 * SIZE_PAGE)
	add x2, x2, #(2 * SIZE_PAGE)
	cmp x0, x3
	b.lo 1b
	ret

_set_up_vbar_and_stacks:
	/* Assumptions:
	 * Running in kernel virtual memory (above VADDR_KERNEL)
	 * x8 => CPU ID
	 * clobbers x0, x1 */
	/* Set vector table */
	adrp x0, _vector_table
	add x0, x0, :lo12:_vector_table
	msr vbar_el1, x0
	
	/* Debug: After setting vector table */
	mov w0, #83
	bl uart_putc_reg

	/* Set TTBR0 translation table to the page that was cleared out earlier. */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	
	/* Strong synchronization to ensure TTBR0 switch completes */
	isb
	dsb sy
	
	/* Debug: After TTBR0 setup */
	mov w0, #84
	bl uart_putc_reg

	/* Set up kernel stack per core - MINIMAL VERSION */
	msr spsel, #1
	
	/* Use absolute minimum stack setup - just set SP to a safe address */
	adrp x0, PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1
	
	/* Inline UART debug immediately after stack setup */
	ldr x3, =(PL011_TTY_BASE + UARTFR_OFFSET)
1:	ldr w4, [x3]
	tst w4, #UARTFR_TXFF
	b.ne 1b
	ldr x3, =PL011_TTY_BASE
	mov w4, #85  /* U marker */
	str w4, [x3]
	
	ret

.size _start, .-_start
_inval_dcache_range:
        /* Invalidate dcache over a range
         * x0 => start address (clobbered)
         * x1 => end address
         * clobbers x0, x2, x3 */
        mrs x2, ctr_el0
        lsr x2, x2, #16
        and x2, x2, #0xf
        mov x3, #4
        lsl x2, x3, x2
1:
        dc ivac, x0
        add x0, x0, x2
        cmp x0, x1
        b.lo 1b
        dsb sy
        isb
        ret


_clean_inval_dcache_range:
        /* Clean + invalidate dcache over a range — ensures any dirty lines
         * are written back to DDR and subsequent reads bypass cache.
         * x0 => start address (clobbered), x1 => end address; clobbers x2,x3 */
        mrs x2, ctr_el0
        lsr x2, x2, #16
        and x2, x2, #0xf
        mov x3, #4
        lsl x2, x3, x2
1:
        dc civac, x0
        add x0, x0, x2
        cmp x0, x1
        b.lo 1b
        dsb sy
        isb
        ret


uart_putc_reg:
1:
        ldr x3, =(PL011_TTY_BASE + UARTFR_OFFSET)
        ldr w4, [x3]
        tst w4, #UARTFR_TXFF
        b.ne 1b
        ldr x3, =PL011_TTY_BASE
        str w0, [x3]
        ret

uart_puthex4:
        and w0, w0, #0xf
        cmp w0, #10
        b.lo 1f
        add w0, w0, #55
        b uart_putc_reg
1:
        add w0, w0, #48
        b uart_putc_reg

uart_puthex64:
        mov x2, x0
        mov x1, #60
1:
        lsr x0, x2, x1
        bl uart_puthex4
        subs x1, x1, #4
        b.pl 1b
        ret

_early_uart_print_tag:
        /* x0 => pointer to a zero-terminated tag */
        ldrb w1, [x0], #1
        cbz w1, 1f
        mov w0, w1
        bl uart_putc_reg
        b _early_uart_print_tag
1:
        ret

_early_exception_common:
        adrp x0, PMAP_COMMON_STACK
        ldr x1, =SIZE_INITIAL_KSTACK
        add sp, x0, x1

        mov w0, #10
        bl uart_putc_reg
        ldr x0, =_early_ex_tag
        bl _early_uart_print_tag
        mov x0, x18
        bl uart_puthex64

        mov w0, #10
        bl uart_putc_reg
        ldr x0, =_early_esr_tag
        bl _early_uart_print_tag
        mrs x0, esr_el1
        bl uart_puthex64

        mov w0, #10
        bl uart_putc_reg
        ldr x0, =_early_elr_tag
        bl _early_uart_print_tag
        mrs x0, elr_el1
        bl uart_puthex64

        mov w0, #10
        bl uart_putc_reg
        ldr x0, =_early_far_tag
        bl _early_uart_print_tag
        mrs x0, far_el1
        bl uart_puthex64

        mov w0, #10
        bl uart_putc_reg
        b .


.align 11
_early_vector_table:
.org (_early_vector_table + 0x000)
        early_vector 0
.org (_early_vector_table + 0x080)
        early_vector 1
.org (_early_vector_table + 0x100)
        early_vector 2
.org (_early_vector_table + 0x180)
        early_vector 3
.org (_early_vector_table + 0x200)
        early_vector 4
.org (_early_vector_table + 0x280)
        early_vector 5
.org (_early_vector_table + 0x300)
        early_vector 6
.org (_early_vector_table + 0x380)
        early_vector 7
.org (_early_vector_table + 0x400)
        early_vector 8
.org (_early_vector_table + 0x480)
        early_vector 9
.org (_early_vector_table + 0x500)
        early_vector 10
.org (_early_vector_table + 0x580)
        early_vector 11
.org (_early_vector_table + 0x600)
        early_vector 12
.org (_early_vector_table + 0x680)
        early_vector 13
.org (_early_vector_table + 0x700)
        early_vector 14
.org (_early_vector_table + 0x780)
        early_vector 15

.align 4
_early_ex_tag:
        .asciz "EX="

.align 4
_early_esr_tag:
        .asciz "ESR="

.align 4
_early_elr_tag:
        .asciz "ELR="

.align 4
_early_far_tag:
        .asciz "FAR="

.ltorg

_initNCpusFlag:
        .word 1

.align 4
.globl hal_cpuInvalDataCacheAll
.type hal_cpuInvalDataCacheAll, %function
hal_cpuInvalDataCacheAll:
.cfi_startproc
	/* 5.3.1 Cleaning and invalidating the caches */
	mov x0, #0x0             /* x0 = Cache level */
	msr csselr_el1, x0       /* 0x0 for L1 Dcache 0x2 for L2 Dcache. */
	mrs x4, ccsidr_el1       /* Read Cache Size ID. */
	and x1, x4, #0x7
	add x1, x1, #0x4         /* x1 = Cache Line Size. */
	mov x3, #0x7fff
	and x2, x3, x4, lsr #13  /* x2 = Cache Set Number – 1. */
	mov x3, #0x3ff
	and x3, x3, x4, lsr #3   /* x3 = Cache Associativity Number – 1. */
	clz w4, w3               /* x4 = way position in the CISW instruction. */
	mov x5, #0               /* x5 = way counter way_loop. */
.Lway_loop:
	mov x6, #0               /* x6 = set counter set_loop. */
.Lset_loop:
	lsl x7, x5, x4
	orr x7, x0, x7           /* Set way. */
	lsl x8, x6, x1
	orr x7, x7, x8           /* Set set. */
	dc isw, x7               /* Invalidate cache line. */
	add x6, x6, #1           /* Increment set counter. */
	cmp x6, x2               /* Last set reached yet? */
	b.le .Lset_loop          /* If not, iterate set_loop, */
	add x5, x5, #1           /* else, next way. */
	cmp x5, x3               /* Last way reached yet? */
	b.le .Lway_loop          /* If not, iterate way_loop. */
	ret
.cfi_endproc
.size hal_cpuInvalDataCacheAll, .-hal_cpuInvalDataCacheAll
.ltorg


#include "hal/aarch64/_exceptions.S"

.section ".bss"
.align 4
_hal_syspageCopied:
	.zero SIZE_PAGE
.size _hal_syspageCopied, . - _hal_syspageCopied
