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

/* Same as DEFAULT_ATTRS but with AttrIndx=1 (MAIR slot 1 = 0x44 = Normal
 * Non-Cacheable, inner+outer). Used for the single TTL3 entry that maps
 * the page containing _hal_syspageCopied. The destination of plo's
 * syspage copy must read back exactly what plo wrote into DDR; on
 * BCM2711 silicon the A72 D-cache observes per-boot-randomized stale
 * lines from the bootcode/start4.elf/armstub firmware chain (or VPU
 * writes into firmware-reserved DRAM windows) at this PA, so the safe
 * approach is to bypass the cache entirely for that one page. See
 * docs/TEMPORARY-FIXES-AND-FUTURE-CLEANUP.md TD-04. */
#define NC_ATTRS 0x707

/* Temporary TTBR0 level-1 RAM block descriptor, Normal Non-Cacheable.
 * Keep the low identity aliases cache-consistent with the TD-04 NC pages
 * while the live boot path still runs with SCTLR.C disabled. */
#define NC_BLOCK_ATTRS 0xf05

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

        /*
         * Drop EL2 -> EL1 if we entered at EL2.
         *
         * Phoenix's armstub (phoenix-armstub8-rpi4.S line 171-175) does
         * `eret` to SPSR_EL3_MODE_EL2H, so on a clean cold boot every
         * core arrives here at EL2. The rest of _init.S writes EL1
         * sysregs (SCTLR_EL1, TTBR0_EL1, TTBR1_EL1, TCR_EL1, MAIR_EL1,
         * VBAR_EL1, CPACR_EL1) which only affect the running execution
         * context if the kernel is actually at EL1. Without this drop,
         * the SCTLR_EL1.M=1 below stages a value that is consulted only
         * by a future eret-to-EL1 (which never comes), so the MMU never
         * actually turns on. This explains why earlier cache-enable
         * attempts (TD-16) silent-hung on real Pi 4: SCTLR_EL1 writes
         * had no observable effect on the running execution context.
         *
         * A72 has no FEAT_VHE / TTBR1_EL2 (armv8.0 baseline) so we
         * cannot keep running at EL2 with our dual-VA pmap. The fix is
         * to drop to EL1 here, mirroring Linux's nvhe init_kernel_el
         * pattern. Cross-OS survey (FreeBSD, NetBSD, OpenBSD, seL4,
         * Genode) confirms every kernel-at-EL1 project drops EL2->EL1
         * before any SCTLR_EL1.M write. See docs/research/el2-to-el1-drop.md
         * and docs/research/boot-mmu-bringup-non-linux.md.
         */
        mrs x0, CurrentEL
        cmp x0, #(2 << 2)            /* CurrentEL.EL == 0b10 ? */
        b.ne el1_entry               /* already at EL1: skip drop */

        /* HCR_EL2.RW = 1: lower exception level (EL1) is AArch64. */
        mov x0, #(1 << 31)
        msr hcr_el2, x0

        /* Stage-2 translation off (we are not a hypervisor). */
        msr vttbr_el2, xzr

        /* Timer plumbing: enable EL1 physical counter (EL0PCTEN bit 0)
         * and EL1 physical timer (EL0PTEN bit 1) as in Linux nvhe
         * __init_el2_timers; zero CNTVOFF so EL1 sees firmware time. */
        mov x0, #3
        msr cnthctl_el2, x0
        msr cntvoff_el2, xzr

        /* Mirror VBAR_EL2 (set by armstub line 179) into VBAR_EL1 so
         * any exception taken between here and the kernel's real
         * VBAR_EL1 install (line ~319) lands in armstub vector code
         * rather than at PC=0. */
        mrs x0, vbar_el2
        msr vbar_el1, x0

        /* Don't trap FP/SIMD/SVE at EL2 on the way through. */
        msr cptr_el2, xzr

        /* SPSR_EL2 = 0x3c5: EL1h, DAIF masked. */
        mov x0, #0x3c5
        msr spsr_el2, x0
        adr x0, el1_entry
        msr elr_el2, x0
        isb
        eret

el1_entry:
        /* DEBUG: Check current exception level (expect 0x4 == EL1). */
        mrs x0, CurrentEL
        uart_putc 90 /* DEBUG: CurrentEL value */

        /* Assumptions:
         * x9 => PA of syspage from PLO */
        uart_putc 75
	/* Mask all interrupts */
	msr daifSet, #0xf

	/* SCTLR_EL1 baseline (MMU + caches OFF for now).
	 *
	 * Value 0x30d4d938 = full A57/A72 RES1 baseline + the bits Phoenix
	 * relies on (matches Linux INIT_SCTLR_EL1_MMU_OFF derivation):
	 *   bit  0 (M):      0 — MMU off
	 *   bit  1 (A):      0 — alignment checks disabled
	 *   bit  2 (C):      0 — D-cache off
	 *   bit  3 (SA):     1 — SP alignment check at EL1
	 *   bit  4 (SA0):    1 — SP alignment check at EL0
	 *   bit  5 (CP15BEN):1 — RES1 on A57/A72 (CP15 barrier enable)
	 *   bit  7 (ITD):    0 — IT disable; left 0 to match Linux
	 *   bit  8 (SED):    1 — SETEND disable
	 *   bit 11 (RES1):   1 — A57/A72 RES1
	 *   bit 12 (I):      0 — I-cache off
	 *   bit 14 (DZE):    1 — DC ZVA at EL0 allowed
	 *   bit 15 (UCT):    1 — CTR_EL0 from EL0 allowed
	 *   bit 16 (nTWI):   1 — don't trap WFI from EL0
	 *   bit 18 (nTWE):   1 — don't trap WFE from EL0
	 *   bit 20 (RES1):   1 — A57/A72 RES1
	 *   bit 22 (RES1):   1 — A57/A72 RES1
	 *   bit 23 (SPAN):   0 — Linux nvhe leaves clear
	 *   bit 26 (UCI):    0 — trap cache maint from EL0
	 *   bit 28 (RES1):   1 — A57/A72 RES1
	 *   bit 29 (RES1):   1 — A57/A72 RES1
	 *   (all other bits 0)
	 *
	 * Earlier value 0x30c0c938 omitted bits 11, 16, 18 — RES1 by ARM
	 * SDEN for A57/A72; QEMU is lenient, real silicon may not be.
	 * Updated per docs/research/boot-mmu-bringup-non-linux.md §4 R1
	 * (cross-OS convergent recommendation: use full RES1 baseline).
	 */
	/* TEMP REVERT 2026-05-08: 0x30d4d938 (RES1 baseline) regressed even
	 * the M-only baseline boot. Reverting to the previously-working
	 * 0x30c0c938. The RES1-baseline change is likely correct in
	 * principle (per docs/research/round3-netbsd-aarch64-rpi4-deep.md
	 * and round3-freebsd-arm64-rpi4-deep.md) but interacts badly with
	 * something else in our boot path; investigate after research wave
	 * synthesis. */
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

	/* Invalidate caches and TLB.
	 *
	 * Step 1 of the round-3 canonical-idiom alignment (see
	 * docs/research/round3-cache-enable-synthesis.md §5):
	 * the kernel must run a full set/way D-cache invalidate at _start
	 * to match the canonical Phoenix A-class idiom (imx6ull, zynq7000,
	 * zynqmp). A72 r0p3 hardware-invalidates L1 at cold reset so this
	 * is a no-op functionally on a clean boot, but it makes the
	 * handoff contract explicit and matches what every other A-class
	 * Phoenix port does. The function walks CLIDR_EL1 levels and
	 * issues `dc isw` per set/way.
	 */
	ic ialluis        /* I-cache invalidate, broadcast IS  */
	tlbi vmalle1is    /* TLB invalidate, broadcast IS      */
	dsb ish
	isb
	/* hal_cpuInvalDataCacheAll uses x0-x10 (including x9). Phoenix's
	 * x9 here holds the syspage PA from plo and is load-bearing
	 * later. Stash it in x28 (caller-saved, not used by the bl
	 * target) across the call. */
	mov x28, x9
	bl hal_cpuInvalDataCacheAll  /* D-cache set/way invalidate-all */
	mov x9, x28
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
        /* SMPEN bit (CPUECTLR_EL1[6]) — left commented because the
         * register access traps from EL1 on A72 (system-register S3_1_*
         * implementation-defined; A72 traps reads/writes from EL1
         * unless explicitly granted). The Phoenix armstub
         * (phoenix-armstub8-rpi4.S) sets this at EL3 before the eret
         * to EL2/EL1. If we ever need to re-assert it, the right place
         * is in the armstub itself, not here.
         *
         * Same applies to A72 erratum 859971 workaround
         * (CPUACTLR_EL1[47] DIS_INSTR_PREFETCH): ARM Trusted Firmware
         * applies this in cortex_a72_reset_func at EL3. Phoenix
         * empirically observed an EL1 trap when attempting it from
         * here (boot stalls at uart_putc 83 = 'S' marker). The
         * workaround needs to migrate to the armstub for the cache
         * enable Stage 1 path to progress past Phase A.
         */
        uart_putc 83 /* DEBUG: SMPEN site (no-op; relies on armstub) */
        uart_putc 84 /* DEBUG: erratum 859971 site (no-op; relies on armstub) */
#endif

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
	mov x3, #(NC_BLOCK_ATTRS) /* Temporary low identity RAM aliases: Normal Non-Cacheable */
	mov x4, #(1 << 30)
	lsr x2, x1, #30
	orr x1, x3, x2, lsl #30
	str x1, [x0, x2, lsl 3]
	add x1, x1, x4
	add x2, x2, #1
	str x1, [x0, x2, lsl 3]

	/* Real Raspberry Pi firmware + kernel8 handoff can place the currently
	 * executing image in a block not covered by syspage->pkernel. The first
	 * instruction fetch after SCTLR_EL1.M is enabled still uses the low PC, so
	 * the temporary TTBR0 identity map must explicitly cover that block. */
	adr x1, .
	lsr x2, x1, #30
	orr x1, x3, x2, lsl #30
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
	 * This is retried after the TD-16 alias cleanup so later cache-enable
	 * attempts have Linux-shaped page-table visibility instead of relying only
	 * on barriers.
	 */
	adrp x0, PMAP_COMMON_KERNEL_TTL2
	adrp x1, PMAP_COMMON_STACK
	bl _inval_dcache_range

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Turn on MMU. Caches stay OFF (C=0, I=0) — known-good baseline.
	 *
	 * Stage 1 cache enable was attempted in many variants; all fail:
	 *   - Phase A (M+I): boot reaches X4/X5/td15/td16, faults at
	 *     syspage.c:476 (hal_syspageRelocate). RES1 baseline didn't
	 *     change it. armstub-side 859971 didn't change it.
	 *   - Phase B (M+C+I): boot hangs at X3 (before X4 marker) —
	 *     immediate post-flip stall, classic "stale D-cache shadows
	 *     page tables" failure mode.
	 *
	 * Conclusion: we are missing something fundamental. Spawned a
	 * deep-research wave (FreeBSD/NetBSD/seL4/ARM ARM authoritative
	 * spec, plo->kernel handoff cache maintenance, BCM2711 firmware
	 * state, diagnostic technique survey). Reverting to known-good
	 * (EL drop + M-only) until research wave returns with concrete
	 * direction.
	 */
	/* Stage 1 cache enable (Step 5 of canonical-idiom alignment plan):
	 * M | C | I in a single SCTLR_EL1 write with canonical barrier ritual.
	 *
	 * Prerequisites in tree:
	 *   - kernel _start invokes hal_cpuInvalDataCacheAll (Step 1)
	 *   - plo flushes full DDR before handoff (Step 2)
	 *   - plo's mmu.c + cache.c generalised to dispatch on currentEL
	 *     (Path A) + TCR_EL2 field-layout fixup for the EL2 path
	 *     (RES0 bit 23 cleared; IPS moved from bits 34:32 -> 18:16)
	 *   - plo runs with MMU+caches ON via hal_memoryInit + canonical
	 *     teardown in hal_cpuJump (Step 3)
	 *   - all TLB/I-cache maintenance uses Inner-Shareable variants
	 *     (vmalle1is, ialluis, dsb ish) — Step 4
	 */
	uart_tag2 88, 51

	/* Pre-flip canonical fence (Linux set_sctlr ritual shape). */
	ic   ialluis
	dsb  ish
	tlbi vmalle1is
	dsb  ish
	isb

	/* MMU only (M=1, C=0, I=0). Matches plo's stable config.
	 * D-cache enable broke exec-kernel inside plo (TD-plo-dcache);
	 * I-cache enable triggers I-fetch garbage (TD-plo-icache). Both
	 * open. Keeping MMU on (essential for any kernel work) and
	 * caches off (slow but correct). */
	mrs  x0, sctlr_el1
	orr  x0, x0, #(1 << 0)    /* SCTLR_EL1.M  — MMU enable */
	msr  sctlr_el1, x0
	isb

	/* Post-flip canonical fence: msr -> isb -> ic iallu -> dsb nsh -> isb. */
	ic   iallu
	dsb  nsh
	isb
	uart_tag2 88, 52

	/* TODO(TD-16-cache-enable): cache enable is parked for now.
	 * Multiple iterations of VA-range invalidation + M|C|I-together
	 * placements all produced layered translation faults on real
	 * Pi 4 (level-3 then level-1 with FAR/ELR moving each time).
	 * Detailed iteration log lives in the comment block right
	 * before the `br x0` to _core_0_virtual below.
	 * Keeping MMU-only enable for now — the boot-correct baseline. */
	uart_tag2 88, 53
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

#if defined(PLO_RPI_MAILBOX_BUFFER_ADDRESS) && (PLO_RPI_MAILBOX_BUFFER_ADDRESS != 0)
	/* TODO(TD-15-mboxprobe): alias the VC4 mailbox buffer page at the
	 * second device TTL3 entry (VA = 0xffffffffffe01000) so the kernel
	 * can read back the pattern plo wrote at PA
	 * PLO_RPI_MAILBOX_BUFFER_ADDRESS just before eret. Mapped Normal
	 * Non-Cacheable (MAIR slot 1, AttrIdx=1) so reads see DDR fresh,
	 * bypassing any stale L1/L2 cache contents. Used by the TD-15
	 * phase 1 drift probe in main.c. Remove when TD-15 phase 2 moves
	 * the buffer out of ARM-usable DRAM. */
	ldr x1, =(PLO_RPI_MAILBOX_BUFFER_ADDRESS | NC_ATTRS)
	str x1, [x0, #8]
#endif

	/* Map TTL3 VADDR_KERNEL ~ (VADDR_KERNEL + 2 MB) => syspage->pkernel ~ (syspage->pkernel + 2 MB) PA */
	adrp x0, PMAP_COMMON_KERNEL_TTL3
	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]
	mov x2, #(DEFAULT_ATTRS)
	orr x1, x1, x2
	bl _fill_page_descr

	/* TD-04 fix: re-map the single page containing _hal_syspageCopied as
	 * Normal Non-Cacheable (MAIR slot 1). _fill_page_descr above wrote
	 * cacheable entries for the entire 2 MB window; we now overwrite the
	 * one entry covering the syspage destination buffer so subsequent
	 * writes during the kernel-side copy and reads in syspage_init()
	 * bypass the A72 D-cache and go directly to DDR. The destination
	 * symbol is .balign'd to SIZE_PAGE in the .bss section so it occupies
	 * exactly one TTL3 entry's worth of address space. The MMU has been
	 * enabled earlier but no high-VA accesses have happened yet, so
	 * there are no TLB entries to invalidate — only the table-walk
	 * barriers below are needed. */
	adrp x0, PMAP_COMMON_KERNEL_TTL3   /* TTL3 base, re-load (x0 was clobbered) */
	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]  /* pkernel = kernel load PA */
	ldr x2, =_hal_syspageCopied         /* link-time high VA of the symbol */
	ldr x3, =VADDR_KERNEL               /* high VA window base */
	sub x4, x2, x3                       /* offset within the 2 MB mapping window */
	lsr x5, x4, #12                      /* page index in TTL3 */
	lsl x5, x5, #3                       /* byte offset (8 B per descriptor) */
	add x6, x1, x4                       /* PA of the page (pkernel + offset) */
	mov x7, #(NC_ATTRS)                  /* attrs with AttrIndx=1 */
	orr x6, x6, x7                        /* full TTL3 entry value */
	str x6, [x0, x5]                      /* override the one entry */

	/* The early C stack is also touched through the high TTBR1 mapping.
	 * Keep it non-cacheable during bring-up for the same BCM2711 stale-line
	 * avoidance reason as _hal_syspageCopied. */
	ldr x2, =PMAP_COMMON_STACK
	ldr x3, =VADDR_KERNEL
	sub x4, x2, x3
	lsr x5, x4, #12
	lsl x5, x5, #3
	add x6, x1, x4
	mov x7, #(NC_ATTRS)
	orr x6, x6, x7
	str x6, [x0, x5]
	add x5, x5, #8
	add x6, x6, #SIZE_PAGE
	str x6, [x0, x5]

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Activate translation through ttbr1_el1 */
	mrs x0, tcr_el1
	bic x0, x0, #(1 << 23)
	msr tcr_el1, x0
	dsb ish
	isb
	tlbi vmalle1is
	dsb ish
	isb

	uart_putc_virt 78

	/* Switch the primary core to the linked higher-half instruction stream.
	 *
	 * The firmware/plo handoff loads the kernel at a low physical address, so
	 * PC remains low across MMU enable. Keeping the rest of the kernel entry
	 * on that low-PC stream is architecturally wrong once TTBR0 is later
	 * replaced: QEMU reports ESR_EL1=0x86000005 (same-EL instruction abort,
	 * level-1 translation fault) immediately after the low-PC branch to main.
	 *
	 * Load the linked virtual target from a literal pool while the low identity
	 * map is still valid, then branch once into the TTBR1 kernel mapping. The
	 * preceding TLB invalidation makes the newly enabled TTBR1 path visible
	 * before the instruction fetch.
	 */

	/* TODO(TD-16-cache-enable): cache enable iteration — three Pi 4
	 * cycles produced a sequence of decoded faults via the no-call
	 * early exception dump:
	 *
	 *   Iteration 1 — set/way invalidation (hal_cpuInvalDataCacheAll)
	 *                + M|C|I together pre-`br x0`:
	 *     EC=0 (Unknown, undecodable instruction). Suspected A72
	 *     erratum #851672 (set/way unreliable on A72).
	 *
	 *   Iteration 2 — VA-range invalidation (dc civac + ic ivau)
	 *                over [pkernel, pkernel + 2 MB) only,
	 *                pre-MMU-enable: silent hang on real Pi between
	 *                X3 and X4 markers (no exception fired).
	 *
	 *   Iteration 3 — VA-range invalidation post-MMU,
	 *                pre-cache-enable, kernel image only,
	 *                C|I (M already set), pre-`br x0`:
	 *     ESR=0x96000003 (Data abort, level-3 translation fault),
	 *     ELR=0xffffffffc00006b0, FAR=0xffffffffc0001890.
	 *     The TTL3 entry for kernel page 1 was read as invalid by
	 *     a literal-pool load right after the high-VA branch.
	 *
	 *   Iteration 4 — added post-enable tlbi vmalle1:
	 *     ESR=0x96000001 (Data abort, level-1 translation fault),
	 *     ELR=0x400658 (low-PA kernel image), FAR=0xfe201018 (PL011).
	 *     The TTBR0 TTL1 entry for the PL011 1 GB block was read as
	 *     invalid during the X7 marker print (`uart_tag2 88, 55`).
	 *
	 *   Iteration 5 — added PT region invalidation (PMAP_COMMON_*)
	 *                in addition to kernel image:
	 *     SAME fault as iteration 4. The page-table region
	 *     invalidation didn't help — the TTBR0 TTL1 still reads
	 *     stale on first walk after cache enable.
	 *
	 * Pattern: every cache-enable attempt on real Pi 4 produces a
	 * walk-time translation fault even though the page tables were
	 * written correctly with caches off (going directly to DDR).
	 * Most plausible remaining hypothesis: A72 speculatively fills
	 * cache lines for these PAs while caches are "off" and those
	 * lines persist past dc civac. Linux's __enable_mmu does cache
	 * maintenance in __cpu_setup BEFORE turning on the MMU; we'd
	 * need to do all our PT writes BEFORE MMU enable too.
	 *
	 * For now, keep MMU enabled but caches off (the boot-correct
	 * baseline that reliably reaches `(psh)%`). The no-call early
	 * exception dump and the VA-range cache helpers stay in place
	 * as building blocks for the next attempt. See TD-16 in
	 * docs/TEMPORARY-FIXES-AND-FUTURE-CLEANUP.md.
	 */

	ldr x0, =_core_0_virtual
	br x0

_core_0_virtual:
	uart_putc_virt 33  /* '!' - primary core now fetches through TTBR1 */

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

	/* E2 probe: stash the original plo syspage PA into x14 so the
	 * post-copy probe can read source bytes via x14 + 0x310 and
	 * compare against the dest-side reads (the copy advances x9 by
	 * `size`, so x9 alone isn't usable post-copy). */
	mov x14, x9

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

	/* (Reverted: experimental pre-copy DEST flush. On real Pi 4 hardware
	 * — but not in QEMU — adding a second `_clean_inval_dcache_range`
	 * call here, over [VADDR_SYSPAGE, VADDR_SYSPAGE+size), caused the
	 * kernel to hang at marker 'V' (line 247) before the X1 MMU-setup
	 * marker. Same image boots cleanly under QEMU. Cache flush of the
	 * dest BSS range likely interacts with whatever already-cached
	 * firmware data sits there in a way that QEMU doesn't model. Left
	 * this comment as a do-not-redo marker; the real fix needs another
	 * approach — see tracking/current-step.md.) */

	/* TD-04 fix: copy through the HIGH-VA mapping of VADDR_SYSPAGE
	 * (loaded from the literal pool), not through a low-PA `adrp +
	 * lo12`. The high-VA TTL3 entry was just overridden to AttrIndx=1
	 * (Normal Non-Cacheable), so str instructions targeting this
	 * pointer write directly to DDR and bypass the A72 D-cache —
	 * exactly the behaviour we need to make the copied bytes immune
	 * to the BCM2711 cache-coherency anomaly observed in the E2 probe.
	 *
	 * (The earlier comment here noted that "instruction-side TTBR1
	 * fetch is broken" on this hardware. That observation still
	 * stands for *instruction* fetches, but the E2 probe explicitly
	 * verified that *data* reads via TTBR1 work — l{} and v{}
	 * markers returned identical bytes every run. Data writes via
	 * TTBR1 are likewise fine; the kernel binary's `_hal_syspageCopied`
	 * symbol is at a high-VA address that the TTL3 we just installed
	 * maps to its load PA.) */
	ldr x1, =VADDR_SYSPAGE
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

	/* Post-copy `_clean_inval_dcache_range` over the LOW-PA dest range.
	 * Empirically required: with TD-04 NC-dest mapping in place, the
	 * COPY's str instructions go via the high-VA NC TTL3 entry directly
	 * to DDR and never touch the A72 D-cache. But the LOW PA still has a
	 * cacheable mapping in SCRATCH_TT, and removing this post-copy flush
	 * regressed the boot to "no kernel UART output at all after plo's
	 * hal: jump exit el1". Probable mechanism: speculative prefetch via
	 * the cacheable LOW-PA mapping populates cache lines from DDR before
	 * the COPY completes; the flush invalidates those stale lines so
	 * later cacheable reads of dest re-fetch from DDR (which has plo's
	 * correct data). The flush is a no-op for the lines that were never
	 * populated, and benign even where the LOW-PA mapping covers
	 * neighbouring kernel data. */
	adrp x0, VADDR_SYSPAGE
	add x0, x0, :lo12:VADDR_SYSPAGE
	ldr x1, [x14, #(SYSPAGE_SIZE_OFFSET)]
	add x1, x0, x1
	bl _clean_inval_dcache_range

	/* TD-16: the syspage copy now only needs TTBR0 for the source reads
	 * above. Drop the low identity map immediately after the copy and
	 * post-copy maintenance so later bootstrap and C code cannot touch the
	 * same PA through both TTBR0 and TTBR1 aliases. */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	dsb sy
	isb

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
	ldr x0, =PMAP_COMMON_STACK
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


/* TODO(TD-16-cache-enable): VA-range I-cache invalidation, matching
 * Linux arm64's __inval_icache_area. Uses ic ivau (Invalidate I-cache
 * line by VA to Point of Unification) per IminLine, which is the
 * architecturally reliable form on Cortex-A72 — set/way-based
 * invalidation triggers erratum #851672 (cache lines can become
 * inconsistent during cache maintenance).
 *
 * The IminLine field of CTR_EL0 (bits 3:0) gives log2(line size in
 * words). Multiply by 4 to get bytes per line.
 *
 * x0 => start address (clobbered), x1 => end address; clobbers x2,x3 */
_inval_icache_range:
        mrs x2, ctr_el0
        and x2, x2, #0xf       /* IminLine = bits 3:0 */
        mov x3, #4
        lsl x2, x3, x2          /* line size in bytes */
1:
        ic ivau, x0
        add x0, x0, x2
        cmp x0, x1
        b.lo 1b
        dsb ish
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

/* TODO(TD-16-exdump): no-call early exception dump.
 *
 * The previous version used many bl/ret to PL011 helpers and to a string
 * walker that read low-PA literal-pool data. When TD-16 cache-enable
 * attempts faulted, those helpers themselves re-faulted, leaving the
 * UART log with only a single "E" repeated forever (recursive exception
 * spam). This shape uses ONLY direct loads/stores and inline branches
 * (no bl, no string-table reads via literal pool, no ret), so it can
 * survive a fault that left SP/x30 corrupted. We get exactly four
 * 16-hex-digit values per fault: vector slot (x18), ESR_EL1, ELR_EL1,
 * FAR_EL1 — separated by '\n'. Kept across builds as the diagnostic
 * floor for any future early-MMU/cache transition.
 *
 * x18 holds the vector slot number set by early_vector. The macro
 * `early_putc_inline` and `early_puthex64_inline` operate purely on
 * x18..x29 so they don't trample the architecturally-defined exception
 * context. They use the early PL011 alias when MMU is on (so device
 * memory) which is mapped in PMAP_COMMON_DEVICES_TTL3; if the fault
 * happened pre-MMU we would not reach this code anyway.
 *
 * Rejected by the previous agent on the grounds that real Pi was
 * "slow not crashed" — but the user clarified the boot was simply
 * waiting on a cache-disabled tty, not in a crashed state, so the
 * diagnostic value of this dump outweighs the fact that any fault
 * here followed by a slow boot is more informative than a fault
 * followed by silence. */
.macro early_putc_inline ch
997991:
        ldr x21, =(PL011_TTY_EARLY_VADDR + UARTFR_OFFSET)
        ldr w22, [x21]
        tst w22, #UARTFR_TXFF
        b.ne 997991b
        ldr x21, =PL011_TTY_EARLY_VADDR
        mov w22, #\ch
        str w22, [x21]
.endm

.macro early_puthex64_inline reg
        mov x23, \reg
        mov x24, #60
998991:
        lsr x25, x23, x24
        and x25, x25, #0xf
        cmp x25, #10
        b.lo 998992f
        add x25, x25, #('a' - 10)
        b 998993f
998992:
        add x25, x25, #'0'
998993:
997992:
        ldr x21, =(PL011_TTY_EARLY_VADDR + UARTFR_OFFSET)
        ldr w22, [x21]
        tst w22, #UARTFR_TXFF
        b.ne 997992b
        ldr x21, =PL011_TTY_EARLY_VADDR
        str w25, [x21]
        subs x24, x24, #4
        b.pl 998991b
.endm

_early_exception_common:
        early_putc_inline 10              /* '\n' */
        early_putc_inline 'E'
        early_putc_inline 'X'
        early_putc_inline '='
        early_puthex64_inline x18

        early_putc_inline 10
        early_putc_inline 'E'
        early_putc_inline 'S'
        early_putc_inline 'R'
        early_putc_inline '='
        mrs x26, esr_el1
        early_puthex64_inline x26

        early_putc_inline 10
        early_putc_inline 'E'
        early_putc_inline 'L'
        early_putc_inline 'R'
        early_putc_inline '='
        mrs x26, elr_el1
        early_puthex64_inline x26

        early_putc_inline 10
        early_putc_inline 'F'
        early_putc_inline 'A'
        early_putc_inline 'R'
        early_putc_inline '='
        mrs x26, far_el1
        early_puthex64_inline x26

        early_putc_inline 10
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
	/* Clean+invalidate all data/unified caches by set/way. The previous
	 * implementation selected L1 only and read CCSIDR_EL1 immediately after
	 * CSSelR_EL1 without the architecturally required ISB, which can make
	 * later D-cache enable experiments hang on real cores or QEMU. */
	dsb sy
	mrs x0, clidr_el1
	and x3, x0, #0x07000000
	lsr x3, x3, #23          /* LoC * 2 */
	cbz x3, .Ldcache_all_done

	mov x10, #0              /* cache level selector: level << 1 */
.Ldcache_level_loop:
	add x2, x10, x10, lsr #1 /* CLIDR cache-type field shift: level * 3 */
	lsr x1, x0, x2
	and x1, x1, #0x7
	cmp x1, #2               /* no data or unified cache at this level */
	b.lt .Ldcache_next_level

	msr csselr_el1, x10
	isb
	mrs x1, ccsidr_el1
	and x2, x1, #0x7
	add x2, x2, #0x4         /* line-length field shift */
	ubfx x4, x1, #3, #10     /* ways - 1 */
	clz w5, w4               /* way field shift */
	ubfx x6, x1, #13, #15    /* sets - 1 */

.Ldcache_set_loop:
	mov x7, x4
.Ldcache_way_loop:
	lsl x8, x7, x5
	orr x8, x10, x8
	lsl x9, x6, x2
	orr x8, x8, x9
	dc cisw, x8
	subs x7, x7, #1
	b.ge .Ldcache_way_loop
	subs x6, x6, #1
	b.ge .Ldcache_set_loop

.Ldcache_next_level:
	add x10, x10, #2
	cmp x3, x10
	b.gt .Ldcache_level_loop

.Ldcache_all_done:
	dsb sy
	isb
	ret
.cfi_endproc
.size hal_cpuInvalDataCacheAll, .-hal_cpuInvalDataCacheAll
.ltorg


.globl _hal_init
.type _hal_init, %function
_hal_init:
	uart_putc_virt 104  /* 'h' - _hal_init reached before C prologue */
	msr spsel, #1
	ldr x0, =PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1
	uart_putc_virt 82   /* 'R' - early stack reset before C prologue */
	ldr x3, =PL011_TTY_EARLY_VADDR
	ldr x0, =_hal_syspageCopied
	add x0, x0, #SIZE_PAGE
	sub x0, x0, #32
	mov x4, #0x51
	str x4, [x0]
	mov w4, #113
	str w4, [x3]        /* 'q' - direct store to known syspage page worked */
	ldr x0, =PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add x0, x0, x1
	sub x0, x0, #32
	mov x4, #0x52
	str x4, [x0]
	mov w4, #119
	str w4, [x3]        /* 'w' - direct store to stack page worked */
	ldr x0, =PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1
	mov x4, #0x53
	str x4, [sp, #-16]!
	mov w4, #117
	str w4, [x3]        /* 'u' - SP pre-index single store worked */
	add sp, sp, #16
	stp x30, x19, [sp, #-32]!
	stp x20, x21, [sp, #16]
	mov w4, #87
	str w4, [x3]        /* 'W' - manual C-style prologue stores worked */
	ldp x20, x21, [sp, #16]
	ldp x30, x19, [sp], #32
	ldr x3, =PL011_TTY_EARLY_VADDR
	mov w4, #88
	str w4, [x3]        /* 'X' - manual prologue restore worked */
	b _hal_init_c
.size _hal_init, .-_hal_init


#include "hal/aarch64/_exceptions.S"

.section ".bss"
/* Page-align so the entire 4 KiB symbol fits exactly into one TTL3 entry.
 * That single entry is overridden in _start to use Normal Non-Cacheable
 * attributes (MAIR slot 1) — see the TD-04 fix block. Without alignment
 * the symbol would straddle two pages and we'd need to override two
 * entries; with alignment one is sufficient and the bookkeeping is
 * trivial. */
.balign SIZE_PAGE
_hal_syspageCopied:
	.zero SIZE_PAGE
.size _hal_syspageCopied, . - _hal_syspageCopied
