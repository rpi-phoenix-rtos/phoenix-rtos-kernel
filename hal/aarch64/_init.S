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
.extern hal_smpPrimaryReady
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
/* Linux-standard TCR: walker uses Inner+Outer WB cacheable +
 * Inner-Shareable attributes for BOTH TTBR0 and TTBR1. */
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
 * Non-Cacheable, inner+outer). The old TD-04 _hal_syspageCopied NC
 * override that used this was deleted (Phase Z3, see _start); the
 * syspage page is now mapped cacheable (DEFAULT_ATTRS). NC_ATTRS now
 * serves only the TD-15 mailbox-buffer alias at DEVICES_TTL3[1]. */
#define NC_ATTRS 0x707

/* C-2 (2026-05-15): Normal Cacheable WBWA, Inner-Shareable TTBR0
 * level-1 RAM block descriptor. Used for the temporary low-PA identity
 * aliases of pkernel/PC/syspage 1 GB blocks set up at the top of
 * el1_entry. Cacheable here so that, once SCTLR.C=1 lands in C-3, the
 * low-PA aliases and the high-VA TTBR1 kernel mappings share the same
 * memory type (per ARM ARM B2.8.1 "Mismatched memory attributes" —
 * any mismatch is UNPREDICTABLE).
 *
 * AttrIdx=0 → MAIR slot 0 = Normal WBWA cacheable. SH=Inner Shareable.
 * AF set, nG (matches earlier NC value of 0xf05 except AttrIdx).
 *
 * The _hal_syspageCopied page is now mapped cacheable (DEFAULT_ATTRS);
 * the old TD-04 NC override was deleted (Phase Z3). plo runs caches-on
 * and does a dc civac DDR teardown before handoff, and the kernel
 * clean+invalidates the syspage range itself, so the cacheable low-PA
 * alias and high-VA kernel mapping share a consistent view. */
#define NC_BLOCK_ATTRS 0xf01

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
#define PMAP_COMMON_KERNEL_TTL3_1 (pmap_common + (2 * SIZE_PAGE))
#define PMAP_COMMON_DEVICES_TTL3 (pmap_common + (3 * SIZE_PAGE))
#define PMAP_COMMON_SCRATCH_TT (pmap_common + (4 * SIZE_PAGE))
#define PMAP_COMMON_SCRATCH_PAGE (pmap_common + (5 * SIZE_PAGE))
#define PMAP_COMMON_STACK (pmap_common + (6 * SIZE_PAGE))
#define VADDR_SYSPAGE (_hal_syspageCopied)

.section .init, "ax"
.global _start
.type _start, %function

.org 0
_start:
	uart_reinit_115200

	/* SMP cold-boot diagnostic (2026-05-23): emit a single 'K'
	 * the very first thing after baud reinit so we can tell, on a
	 * silent "smp release-2 → kernel entry → (nothing)" hang,
	 * whether kernel _start ran at all. If 'K' appears in the
	 * UART log, _start started and the hang is later; if 'K' is
	 * absent, kernel _start never ran (firmware/plo-seam issue,
	 * not a kernel-side bug). */
	uart_putc 75

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
        /* Assumptions:
         * x9 => PA of syspage from PLO */

	/* Mask all interrupts */
	msr daifSet, #0xf

	/* SMP Phase D: secondaries arrive here from armstub's
	 * `br x4` (where x4 = kernel entry PA, written by plo's
	 * release-2). Armstub clears x0-x3 before the branch but leaves
	 * x9 unspecified, so the assumption `x9 = syspage PA` only holds
	 * for the primary (which entered via plo's hal_exitToEL1).
	 *
	 * Fix: plo's hal_cpuJump writes the syspage PA to PA 0xD8 (=
	 * armstub spin_cpu0, which no secondary ever reads from its
	 * WFE-spin since cpu0 doesn't spin) just before release-2.
	 * Reload x9 from there if cpu_id != 0 so secondaries see the
	 * same x9 the primary already has. At this point MMU is still
	 * off (or low-PA identity mapped) so a direct PA 0xD8 load works.
	 * Clobbers x6 here only; the shared code below re-reads MPIDR
	 * later for its own cbnz logic. */
	mrs x6, mpidr_el1
	and x6, x6, #0xff
	cbz x6, 9501f
	mov x7, #0xd8
	ldr x9, [x7]
9501:

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
	/* TEMP REVERT 2026-05-16: 0x30d4d938 (full A57/A72 RES1 baseline)
	 * still regressed on real Pi 4 even after the 4 MiB early-kernel map
	 * fix, dying after X4 with ESR=0x02000000. Keep the previously-working
	 * M-only baseline while cache enable is isolated.
	 */
	ldr x0, =0x30c0c938
	msr sctlr_el1, x0

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

	/* SMPEN bit (CPUECTLR_EL1[6]) and A72 erratum workarounds
	 * (859971, 1319367) trap from EL1 on A72 r0p3. They are applied
	 * at EL3 in the Pi 4 armstub (phoenix-armstub8-rpi4.S). Per
	 * docs/rpi4-os-development-guide.md, those EL3 writes are the
	 * single most important hardware-init step for this board. */

	/* Set up MMU */
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

	adr x0, _early_vector_table
	msr vbar_el1, x0

	adrp x0, PMAP_COMMON_KERNEL_TTL2
	msr ttbr1_el1, x0

	/* C-1 (2026-05-15, Linux __cpu_setup pattern): construct ALL kernel
	 * TTBR1 page tables BEFORE SCTLR.M=1. The walker then sees a fully-
	 * populated table from the very first translation request; no
	 * post-MMU PT writes are needed. Replaces the legacy post-MMU PT
	 * setup that lived at lines 551-646; that block is deleted below.
	 *
	 * For Pi 4, every store with MMU off is treated as Normal-NC
	 * (EL1 default per ARM ARM B2.4.6) → goes direct to DDR. The
	 * walker will then read these entries through TCR-controlled
	 * cacheable inner-shareable walks once SCTLR.M=1, observing a
	 * coherent view because the bytes are already in DDR.
	 *
	 * Cores 1-3 also reach this block; they write the same values
	 * (race-free same-write convergence). The MPIDR check that traps
	 * them to _other_core_trap remains post-MMU. */

	/* Zero KERNEL_TTL2 */
	adrp x0, PMAP_COMMON_KERNEL_TTL2
	bl _fill_page_zero

	/* TTL2[0] = KERNEL_TTL3 | table-descriptor */
	adrp x1, PMAP_COMMON_KERNEL_TTL3
	orr x1, x1, #0x3
	str x1, [x0]

	/* TTL2[1] = KERNEL_TTL3_1 | table-descriptor */
	adrp x1, PMAP_COMMON_KERNEL_TTL3_1
	orr x1, x1, #0x3
	str x1, [x0, #8]

	/* TTL2[last] = DEVICES_TTL3 | table-descriptor */
	add x2, x0, #(SIZE_PAGE - 8)
	adrp x0, PMAP_COMMON_DEVICES_TTL3
	orr x1, x0, #0x3
	str x1, [x2]

	/* Zero DEVICES_TTL3 (x0 still holds its PA) */
	bl _fill_page_zero

#if defined(PL011_TTY_EARLY_VADDR)
	/* DEVICES_TTL3[0] = PL011_TTY_BASE | EARLY_UART_DEVICE_DESCR */
	ldr x1, =(PL011_TTY_BASE | EARLY_UART_DEVICE_DESCR)
	str x1, [x0]
#endif

#if defined(PLO_RPI_MAILBOX_BUFFER_ADDRESS) && (PLO_RPI_MAILBOX_BUFFER_ADDRESS != 0)
	/* TD-15 mailbox-buffer NC alias at DEVICES_TTL3[1] */
	ldr x1, =(PLO_RPI_MAILBOX_BUFFER_ADDRESS | NC_ATTRS)
	str x1, [x0, #8]
#endif

	/* Fill KERNEL_TTL3 with descriptors covering
	 * VADDR_KERNEL..VADDR_KERNEL+4MB -> pkernel..pkernel+4MB.
	 * The current Pi 4 kernel image extends beyond the first 2 MB,
	 * so the early high mapping must cover at least two TTL3 pages
	 * before D-cache is enabled and normal C calls resume.
	 */
	adrp x0, PMAP_COMMON_KERNEL_TTL3
	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]
	mov x2, #(DEFAULT_ATTRS)
	orr x1, x1, x2
	bl _fill_page_descr

	adrp x0, PMAP_COMMON_KERNEL_TTL3_1
	ldr x1, [x9, #(SYSPAGE_PKERNEL_OFFSET)]
	add x1, x1, #0x200000
	mov x2, #(DEFAULT_ATTRS)
	orr x1, x1, x2
	bl _fill_page_descr

	/* Phase Z3 (2026-05-17): TD-04 NC override DELETED.
	 *
	 * Previously this overrode the one TTL3 entry covering
	 * _hal_syspageCopied to Normal Non-Cacheable (NC_ATTRS) to work
	 * around a BCM2711 cache-coherency anomaly: plo wrote the syspage
	 * to that page with caches off, kernel read it back through a
	 * cacheable mapping, and stale firmware-era cache lines for the
	 * PA produced bit-level non-determinism in the read-back bytes.
	 *
	 * With Phase Z1 the contract is now canonical-zynqmp: plo runs
	 * caches-on, writes to syspage go through the A72 D-cache, and
	 * plo's teardown does dc civac over all DDR (clean+invalidate),
	 * so the kernel reads the same bytes plo wrote. No NC override
	 * needed.
	 *
	 * If real-Pi UART shows syspage-relocation failures or
	 * _hal_syspageCopied reads coming back wrong under Phase Z, this
	 * is the first hunk to revert — but the canonical pattern is
	 * supposed to make it unnecessary.
	 */

	/* Activate TTBR1 walks (clear EPD1) — still pre-MMU. */
	mrs x0, tcr_el1
	bic x0, x0, #(1 << 23)
	msr tcr_el1, x0

	/* dsb sy: with MMU off, stores use Normal-NC; full-system barrier
	 * is the safe choice to drain the write buffer to DDR before MMU
	 * enable. */
	dsb sy
	isb

	/* Linux arm64 explicitly invalidates page tables populated with the MMU
	 * disabled to discard any speculatively loaded cache lines before the
	 * page-table walker starts using them. Do the same for the contiguous
	 * early kernel MMU region:
	 *   PMAP_COMMON_KERNEL_TTL2
	 *   PMAP_COMMON_KERNEL_TTL3
	 *   PMAP_COMMON_KERNEL_TTL3_1
	 *   PMAP_COMMON_DEVICES_TTL3
	 *   PMAP_COMMON_SCRATCH_TT
	 *   PMAP_COMMON_SCRATCH_PAGE
	 * Performed after the writes above so any speculative cache fills
	 * during the writes are discarded; subsequent walker reads then
	 * hit RAM via TCR-controlled cacheable inner-shareable walks.
	 */
	adrp x0, PMAP_COMMON_KERNEL_TTL2
	adrp x1, PMAP_COMMON_STACK
	bl _inval_dcache_range

	/* Ensure changes to translation tables are visible */
	dsb ish
	isb

	/* Enable MMU+D-cache+I-cache (SCTLR.M|C|I) in a single write below;
	 * A72; canonical barrier ritual with a defensive set/way invalidate
	 * immediately prior. (TD-16 RESOLVED 2026-05-17.) */
	/* Pre-flip canonical fence (Linux set_sctlr ritual shape). */
	ic   ialluis
	dsb  ish
	tlbi vmalle1is
	dsb  ish
	isb

	/* Defensive set/way D-cache invalidate immediately before the M|C|I
	 * flip: the A72 prefetchers may speculatively populate lines even
	 * with SCTLR.C=0, so re-invalidate to guarantee the cache is empty
	 * when caches go live (the earlier _start invalidate ran long ago). */
	mov x28, x9                       /* preserve syspage PA */
	bl  hal_cpuInvalDataCacheAll
	mov x9, x28
	dsb ish
	isb

	/* Single-shot M|C|I — canonical zynqmp aarch64 pattern. Pairs with
	 * plo running caches-on with a dc civac DDR teardown before handoff,
	 * so the kernel enables MMU+caches in one SCTLR write. */
	mrs  x0, sctlr_el1
	orr  x0, x0, #(1 << 0)    /* SCTLR_EL1.M  — MMU enable */
	orr  x0, x0, #(1 << 2)    /* SCTLR_EL1.C  — D-cache enable */
	orr  x0, x0, #(1 << 12)   /* SCTLR_EL1.I  — I-cache enable */
	msr  sctlr_el1, x0
	isb

	/* Post-flip canonical fence: msr -> isb -> ic iallu -> dsb nsh -> isb. */
	ic   iallu
	dsb  nsh
	isb

	/* Post-flip coherency preparation: while the low handoff alias is
	 * still valid, clean the kernel's low-PA text range to PoU and
	 * invalidate the matching I-cache range. The late C helper cannot
	 * touch VA 0x400000 after the low bootstrap mapping is gone. */
	ldr x0, =0x400000
	ldr x1, =0x640000
	bl _clean_dcache_pou_range
	ldr x0, =0x400000
	ldr x1, =0x640000
	bl _inval_icache_range

	/* Translation is now taking place through PMAP_COMMON_SCRATCH_TT.
	 * Before we can jump to kernel in virtual memory we must set up
	 * PMAP_COMMON_KERNEL_TTL2 table. */

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

	/* Cache enable (TD-16 RESOLVED 2026-05-17): SCTLR_EL1.M|C|I is set
	 * in a single write inside el1_entry after the armstub L2CTLR_EL1
	 * + A72 erratum 1319367 fix landed in project commit dde9bb5. The
	 * historical iteration-by-iteration narrative of the pre-resolution
	 * cache-enable attempts is preserved in
	 * docs/TEMPORARY-FIXES-AND-FUTURE-CLEANUP.md under TD-16. */

	ldr x0, =_core_0_virtual
	br x0

_core_0_virtual:

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

	/* Copy through the HIGH-VA mapping of VADDR_SYSPAGE (loaded from the
	 * literal pool), not through a low-PA `adrp + lo12`. The high-VA TTL3
	 * entry is now mapped cacheable (DEFAULT_ATTRS); the old TD-04 NC
	 * override was deleted (Phase Z3). Coherency relies on plo's caches-on
	 * + dc civac DDR teardown plus the _clean_inval_dcache_range over
	 * [x9, x9+size) above, so the copied bytes are authoritative in DDR.
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

	/* Syspage copy complete. The kernel now only needs TTBR0 for the
	 * source reads above; drop the low identity map immediately so
	 * later bootstrap and C code cannot touch the same PA through
	 * both TTBR0 and TTBR1 aliases. */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	dsb sy
	isb

	/* Set vector table */
	adrp x0, _vector_table
	add x0, x0, :lo12:_vector_table
	msr vbar_el1, x0

	/* Set TTBR0 translation table (kernel scratch page).
	 * Strong synchronization around the swap. */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	isb
	dsb sy

	/* Set up kernel stack now that MMU is fully active. */
	msr spsel, #1
	ldr x0, =PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1

	/* Branch to C kernel entry. */
	b main


_other_core_trap:
	dsb ish
	wfe
	ldr w0, [x1, #:lo12:nCpusStarted]
	cbz w0, _other_core_trap

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
#if NUM_CPUS == 1
	/* NUM_CPUS=1: park secondaries here forever. */
	msr daifSet, #0xf
9100:
	wfi
	b 9100b
#endif

	/* SMP-D-5 primary-ready wait. Spin until primary publishes
	 * hal_smpPrimaryReady != 0 (from main() after vm/proc/threads
	 * init). Without this gate, secondaries' GIC/atomic activity
	 * races primary's vm subsystem and hangs the boot.
	 *
	 * Read via TTBR1 high-VA mapping (MMU is on here, shared MMU
	 * setup from lines 0-664 set up TTBR1). x8 is preserved
	 * (cpu_id). Clobbers x9, x10. */
9120:
	adrp x9, hal_smpPrimaryReady
	add x9, x9, :lo12:hal_smpPrimaryReady
	ldr w10, [x9]
	cbnz w10, 9121f
	wfe
	b 9120b
9121:
	dsb ish
	isb

	bl _set_up_vbar_and_stacks
	bl _hal_interruptsInitPerCPU
	bl _hal_cpuInit

	/* SMP Phase D-8 active: secondaries arm CNTV via
	 * _hal_timerInitPerCPU with the 10-second hal_smpFirstIntervalUs
	 * override set by main(). After unmasking DAIF the core takes
	 * its first timer PPI at t≈10 s, well after primary's boot
	 * has reached fbcon. threads_timeintr's secondary path then
	 * re-arms CNTV at SYSTICK_INTERVAL cadence so the secondary
	 * actually contributes to scheduling. */
	bl _hal_timerInitPerCPU
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
	/* Per-CPU bring-up. Called only by secondaries (primary inlines the
	 * equivalent setup before `b main`). Assumptions:
	 *   - Running in kernel virtual memory (above VADDR_KERNEL).
	 *   - x8 holds this CPU's MPIDR Aff0 (set at line ~662 in the shared
	 *     entry path before the cbnz that branches secondaries here).
	 *
	 * Critical fix (SMP Phase D, 2026-05-22): this function previously
	 * called `bl uart_putc_reg` twice for "S"/"T" debug markers without
	 * saving LR, so the inner `bl`s clobbered x30 and the function's
	 * own `ret` jumped back into its own body — secondaries livelocked
	 * mid-bringup, never reaching _hal_interruptsInitPerCPU /
	 * _hal_timerInitPerCPU. The debug markers are removed entirely
	 * (Phase A/B already established that secondaries do enter this
	 * code path), so no inner `bl` is needed.
	 *
	 * Critical fix (same commit): every secondary used to set SP to
	 * `PMAP_COMMON_STACK + SIZE_INITIAL_KSTACK` — the top of CPU 0's
	 * stack slot, shared by all secondaries. C frames from concurrent
	 * secondaries clobbered each other. pmap.c reserves
	 * `stack[NUM_CPUS][SIZE_INITIAL_KSTACK]` starting at
	 * PMAP_COMMON_STACK, so we just index by CPU ID:
	 *   SP = PMAP_COMMON_STACK + (cpu_id + 1) * SIZE_INITIAL_KSTACK
	 * (i.e. top of stack[cpu_id], growing down into its slot).
	 *
	 * clobbers x0, x1, x2 */

	/* Vector table */
	adrp x0, _vector_table
	add x0, x0, :lo12:_vector_table
	msr vbar_el1, x0
	isb

	/* TTBR0 = scratch (kept idempotent: _other_core_trap already wrote
	 * the same value before branching here, but a second write costs
	 * nothing and keeps the function self-contained). */
	adrp x0, PMAP_COMMON_SCRATCH_PAGE
	msr ttbr0_el1, x0
	isb
	dsb sy

	/* Switch to SP_EL1 and install this CPU's dedicated stack. */
	msr spsel, #1
	adrp x0, PMAP_COMMON_STACK
	add x1, x8, #1
	mov x2, SIZE_INITIAL_KSTACK
	mul x1, x1, x2
	add sp, x0, x1

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


_clean_dcache_pou_range:
        /* Clean dcache over a range to Point of Unification. This is the
         * data-side half of the standard text coherency sequence before
         * enabling SCTLR.I when the image was loaded by earlier firmware/plo
         * stages through potentially different aliases.
         * x0 => start address (clobbered), x1 => end address; clobbers x2,x3 */
        mrs x2, ctr_el0
        lsr x2, x2, #16
        and x2, x2, #0xf
        mov x3, #4
        lsl x2, x3, x2
1:
        dc cvau, x0
        add x0, x0, x2
        cmp x0, x1
        b.lo 1b
        dsb ish
        isb
        ret


/* VA-range I-cache invalidation, matching Linux arm64's
 * __inval_icache_area. Uses ic ivau (Invalidate I-cache line by VA to
 * Point of Unification) per IminLine, which is the architecturally
 * reliable form on Cortex-A72 — set/way-based invalidation triggers
 * erratum #851672 (cache lines can become inconsistent during cache
 * maintenance).
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

/* Permanent early-exception diagnostic: no-call early exception dump.
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
	/* Invalidate (DISCARD, do NOT clean) all data/unified caches by
	 * set/way. Matches Linux arm64 `__asm_invalidate_dcache_all` (uses
	 * `dc isw`, not `dc cisw`).
	 *
	 * Rationale: this function is called from kernel _start with
	 * SCTLR.C=0 (no kernel cache fills yet). Any dirty cache lines
	 * present at this point must be stale firmware-era lines left over
	 * from the BCM2711 VC4 boot chain — NOT data the kernel wrote. Using
	 * `dc cisw` (clean+invalidate) would WRITE THOSE STALE LINES BACK to
	 * DDR over plo's just-loaded kernel image / syspage / PT memory,
	 * corrupting everything. `dc isw` discards them, which is the
	 * correct behavior for a cold-start cache cleanup.
	 *
	 * Symptom of the cisw bug pre-fix: with M|C enabled, the kernel
	 * reached _hal_init_c entry but the first stack store (stp x30,
	 * x19, [sp, #-32]!) hung silently, no fault. Theory: cisw wrote
	 * stale lines back over the kernel image, corrupting the
	 * instruction stream that the I-fetch then read uncached from DDR.
	 *
	 * Previous "clean+invalidate" implementation correctly selected
	 * every cache level (LoC * 2 walk, ISB after CSSelR_EL1) — only the
	 * single instruction below is wrong. */
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
	dc isw, x8
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


/* Deferred-cache-enable helpers (hal_cpuEnableDCache /
 * hal_cpuEnableICache / hal_cpuEnableCaches) lived here during the
 * cache-coherency investigation. With the 2026-05-17 armstub fix
 * (CPUACTLR_EL1[46] erratum 1319367 + L2CTLR_EL1 BCM2711 timing)
 * the kernel enables M|C|I in one SCTLR write inside el1_entry above;
 * the deferred helpers were dead code (no callers) and have been
 * removed. Restore from history if a future use case needs them. */


.globl _hal_init
.type _hal_init, %function
_hal_init:
	msr spsel, #1
	ldr x0, =PMAP_COMMON_STACK
	mov x1, SIZE_INITIAL_KSTACK
	add sp, x0, x1
	b _hal_init_c
.size _hal_init, .-_hal_init


#include "hal/aarch64/_exceptions.S"

.section ".bss"
/* Page-align so the entire 4 KiB symbol fits exactly into one TTL3 entry.
 * The page is mapped cacheable (DEFAULT_ATTRS) like the rest of the
 * kernel; the old TD-04 Normal-Non-Cacheable override was deleted
 * (Phase Z3). Alignment is kept for clean single-entry TTL3 bookkeeping
 * (the symbol does not straddle two pages). */
.balign SIZE_PAGE
_hal_syspageCopied:
	.zero SIZE_PAGE
.size _hal_syspageCopied, . - _hal_syspageCopied
