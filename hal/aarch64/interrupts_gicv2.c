/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Interrupt handling for ARM GIC v1 or v2
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Hubert Buczynski, Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/aarch64/aarch64.h"

#include "hal/cpu.h"
#include "hal/console.h"
#include "hal/spinlock.h"
#include "hal/interrupts.h"
#include "hal/list.h"
#include "hal/timer.h"

#include "proc/userintr.h"

#include "dtb.h"
#include "interrupts_gicv2.h"
#include "arch/pmap.h"
#include "config.h"

#include "perf/trace-events.h"
#include "lib/lib.h"

#include "include/errno.h"

#define SPI_FIRST_IRQID 32

#define SGI_FLT_USE_LIST   0 /* Send SGI to CPUs according to targetList */
#define SGI_FLT_OTHER_CPUS 1 /* Send SGI to all CPUs except the one that called this function */
#define SGI_FLT_THIS_CPU   2 /* Send SGI to the CPU that called this function */

#define DEFAULT_CPU_MASK ((1U << NUM_CPUS) - 1U)
#define DEFAULT_PRIORITY 0x80


enum {
	/* Distributor registers */
	gicd_ctlr = 0x0,
	gicd_typer,
	gicd_iidr,
	gicd_igroupr0 = 0x20,     /* 6 registers */
	gicd_isenabler0 = 0x40,   /* 6 registers */
	gicd_icenabler0 = 0x60,   /* 6 registers */
	gicd_ispendr0 = 0x80,     /* 6 registers */
	gicd_icpendr0 = 0xa0,     /* 6 registers */
	gicd_isactiver0 = 0xc0,   /* 6 registers */
	gicd_icactiver0 = 0xe0,   /* 6 registers */
	gicd_ipriorityr0 = 0x100, /* 48 registers */
	gicd_itargetsr0 = 0x200,  /* 48 registers */
	gicd_icfgr0 = 0x300,      /* 12 registers */
	gicd_ppisr = 0x340,
	gicd_spisr0, /* 5 registers */
	gicd_sgir = 0x3c0,
	gicd_cpendsgir0 = 0x3c4, /* 4 registers */
	gicd_spendsgir0 = 0x3c8, /* 4 registers */
	gicd_pidr4 = 0x3f4,      /* 4 registers */
	gicd_pidr0 = 0x3f8,      /* 4 registers */
	gicd_cidr0 = 0x3fc,      /* 4 registers */
};


enum {
	/* CPU interface registers */
	gicc_ctlr = 0x0,
	gicc_pmr,
	gicc_bpr,
	gicc_iar,
	gicc_eoir,
	gicc_rpr,
	gicc_hppir,
	gicc_abpr,
	gicc_aiar,
	gicc_aeoir,
	gicc_ahppir,
	gicc_apr0 = 0x34,
	gicc_nsapr0 = 0x38,
	gicc_iidr = 0x3f,
};


static struct {
	volatile u32 *gicd;
	volatile u32 *gicc;
	spinlock_t spinlock[SIZE_INTERRUPTS];
	intr_handler_t *handlers[SIZE_INTERRUPTS];
	unsigned int counters[SIZE_INTERRUPTS];
	int trace_irqs;
} interrupts_common;


void _hal_interruptsInitPerCPU(void);

int threads_schedule(unsigned int n, cpu_context_t *context, void *arg);


/* parasoft-begin-suppress MISRAC2012-RULE_2_2 MISRAC2012-RULE_8_4 "Function is used externally within assembler code" */
int interrupts_dispatch(unsigned int n, cpu_context_t *ctx)
{
	intr_handler_t *h;
	unsigned int reschedule = 0;
	spinlock_ctx_t sc;
	int trace;

	u32 ciarValue = *(interrupts_common.gicc + gicc_iar);
	n = ciarValue & 0x3ffU;

	if (n >= SIZE_INTERRUPTS) {
		return 0;
	}

	trace = interrupts_common.trace_irqs != 0 && n != hal_timerIrq();
	if (trace != 0) {
		trace_eventInterruptEnter(n);
	}

	hal_spinlockSet(&interrupts_common.spinlock[n], &sc);

	interrupts_common.counters[n]++;

	h = interrupts_common.handlers[n];
	if (h != NULL) {
		do {
			reschedule |= (unsigned int)h->f(n, ctx, h->data);
			h = h->next;
		} while (h != interrupts_common.handlers[n]);
	}

	if (reschedule != 0U) {
		(void)threads_schedule(n, ctx, NULL);
	}

	*(interrupts_common.gicc + gicc_eoir) = ciarValue;

	hal_spinlockClear(&interrupts_common.spinlock[n], &sc);

	if (trace != 0) {
		trace_eventInterruptExit(n);
	}

	return (int)reschedule;
}


static void interrupts_enableIRQ(unsigned int irqn)
{
	unsigned int irq_reg = irqn / 32U;
	unsigned int irq_offs = irqn % 32U;
	*(interrupts_common.gicd + (u32)gicd_isenabler0 + irq_reg) = (u32)1U << irq_offs;
}


static void interrupts_disableIRQ(unsigned int irqn)
{
	unsigned int irq_reg = irqn / 32U;
	unsigned int irq_offs = irqn % 32U;
	*(interrupts_common.gicd + (u32)gicd_icenabler0 + irq_reg) = (u32)1U << irq_offs;
}


static void interrupts_setConf(unsigned int irqn, u32 conf)
{
	unsigned int irq_reg = irqn / 16U;
	unsigned int irq_offs = (irqn % 16U) * 2U;
	u32 mask;

	mask = *(interrupts_common.gicd + gicd_icfgr0 + irq_reg) & ~(0x3U << irq_offs);
	*(interrupts_common.gicd + gicd_icfgr0 + irq_reg) = mask | ((conf & 0x3U) << irq_offs);
}


static void interrupts_setGroup(unsigned int irqn, u32 group)
{
	unsigned int irq_reg = irqn / 32U;
	unsigned int irq_offs = irqn % 32U;
	u32 mask;

	mask = *(interrupts_common.gicd + gicd_igroupr0 + irq_reg) & ~((u32)1U << irq_offs);
	*(interrupts_common.gicd + gicd_igroupr0 + irq_reg) = mask | ((group & 0x1U) << irq_offs);
}


void interrupts_setCPU(unsigned int irqn, unsigned int cpuID)
{
	unsigned int irq_reg = irqn / 4U;
	unsigned int irq_offs = (irqn % 4U) * 8U;
	u32 mask;

	mask = *(interrupts_common.gicd + gicd_itargetsr0 + irq_reg) & ~(0xffU << irq_offs);
	*(interrupts_common.gicd + gicd_itargetsr0 + irq_reg) = mask | ((cpuID & 0xffU) << irq_offs);
}


static void interrupts_setPriority(unsigned int irqn, u32 priority)
{
	unsigned int irq_reg = irqn / 4U;
	unsigned int irq_offs = (irqn % 4U) * 8U;
	u32 mask = *(interrupts_common.gicd + gicd_ipriorityr0 + irq_reg) & ~(0xffU << irq_offs);

	*(interrupts_common.gicd + gicd_ipriorityr0 + irq_reg) = mask | ((priority & 0xffU) << irq_offs);
}


u32 interrupts_getPending(unsigned int irqn)
{
	unsigned int irq_reg = irqn / 32U;
	unsigned int irq_offs = irqn % 32U;

	return (*(interrupts_common.gicd + gicd_ispendr0 + irq_reg) >> irq_offs) & 0x1U;
}


u32 interrupts_getPrivatePending(unsigned int irqn)
{
	if ((irqn < 16U) || (irqn >= 32U)) {
		return 0U;
	}

	return (*(interrupts_common.gicd + gicd_ppisr) >> (irqn - 16U)) & 0x1U;
}


int hal_interruptsSetHandler(intr_handler_t *h)
{
	spinlock_ctx_t sc;

	if ((h == NULL) || (h->f == NULL) || (h->n >= SIZE_INTERRUPTS)) {
		return -EINVAL;
	}

	hal_spinlockSet(&interrupts_common.spinlock[h->n], &sc);
	HAL_LIST_ADD(&interrupts_common.handlers[h->n], h);

	if (h->n >= 16U) {
		interrupts_setConf(h->n, (u32)_interrupts_gicv2_classify(h->n));
	}

	/* The generic handoff path reaches the kernel in non-secure EL1, so the
	 * selected architectural timer PPI must be placed in Group 1. */
	if (h->n == hal_timerIrq()) {
		interrupts_setGroup(h->n, TIMER_IRQ_GROUP);
	}

	interrupts_setPriority(h->n, DEFAULT_PRIORITY);
	if (h->n >= SPI_FIRST_IRQID) {
		interrupts_setCPU(h->n, DEFAULT_CPU_MASK);
	}
	interrupts_enableIRQ(h->n);

	hal_spinlockClear(&interrupts_common.spinlock[h->n], &sc);

	return 0;
}


char *hal_interruptsFeatures(char *features, size_t len)
{
	(void)hal_strncpy(features, "Using GIC interrupt controller", len);
	/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "`len` is always non-zero." */
	features[len - 1U] = '\0';

	return features;
}


int hal_interruptsDeleteHandler(intr_handler_t *h)
{
	spinlock_ctx_t sc;

	if ((h == NULL) || (h->f == NULL) || (h->n >= SIZE_INTERRUPTS)) {
		return -1;
	}

	hal_spinlockSet(&interrupts_common.spinlock[h->n], &sc);
	HAL_LIST_REMOVE(&interrupts_common.handlers[h->n], h);

	if (interrupts_common.handlers[h->n] == NULL) {
		interrupts_disableIRQ(h->n);
	}

	hal_spinlockClear(&interrupts_common.spinlock[h->n], &sc);

	return 0;
}


void _hal_interruptsTrace(int enable)
{
	interrupts_common.trace_irqs = !!enable;
}


/* Function initializes interrupt handling */
void _hal_interruptsInit(void)
{
	u32 i;
	addr_t gicc, gicd;

	interrupts_common.trace_irqs = 0;

	dtb_getGIC(&gicc, &gicd);
	interrupts_common.gicd = _pmap_halMapDevice(gicd, 0, SIZE_PAGE);
	interrupts_common.gicc = _pmap_halMapDevice(gicc, 0, SIZE_PAGE);

	for (i = 0; i < SIZE_INTERRUPTS; ++i) {
		interrupts_common.handlers[i] = NULL;
		interrupts_common.counters[i] = 0;
		hal_spinlockCreate(&interrupts_common.spinlock[i], "interrupts");
	}

	/* Clear pending and disable interrupts */
	for (i = 0; i < (SIZE_INTERRUPTS + 31U) / 32U; i++) {
		*(interrupts_common.gicd + gicd_icenabler0 + i) = 0xffffffffU;
		*(interrupts_common.gicd + gicd_icpendr0 + i) = 0xffffffffU;
		*(interrupts_common.gicd + gicd_icactiver0 + i) = 0xffffffffU;
	}

	for (i = 0; i < 4U; i++) {
		*(interrupts_common.gicd + gicd_cpendsgir0 + i) = 0xffffffffU;
	}

	/* Disable distributor */
	*(interrupts_common.gicd + gicd_ctlr) &= ~0x3U;

	/* TODO: detect if we are in secure or non-secure mode and if secure, configure interrupt groups */

	/* Set default priorities - 128 for the SGI (IRQID: 0 - 15), PPI (IRQID: 16 - 31), SPI (IRQID: 32 - 188) */
	for (i = 0; i < SIZE_INTERRUPTS; ++i) {
		interrupts_setPriority(i, DEFAULT_PRIORITY);
	}

	/* Set required configuration and CPU mask */
	for (i = SPI_FIRST_IRQID; i < SIZE_INTERRUPTS; ++i) {
		interrupts_setConf(i, (u32)_interrupts_gicv2_classify(i));
		interrupts_setCPU(i, DEFAULT_CPU_MASK);
	}

	/* enable_secure = 1 */
	*(interrupts_common.gicd + gicd_ctlr) |= 0x3U;

	_hal_interruptsInitPerCPU();
}


/* SMP Phase D observability: per-CPU bring-up counters.
 *   [cpuId] in interruptsInitPerCpu = entered _hal_interruptsInitPerCPU
 *   [cpuId] in interruptsEnabledPpi = enabled the timer PPI in
 *                                     GICD_ISENABLER0 (secondaries only)
 * primary's main_initthr prints these after the spawn loop. */
volatile unsigned int hal_smpInterruptsInitPerCpuCount[8];
volatile unsigned int hal_smpInterruptsEnabledPpiCount[8];

/* SMP-D primary-ready flag: secondaries spin-wait on this in
 * _other_core_virtual (assembly) before invoking the per-CPU C
 * init helpers. Primary sets it to 1 from main() after vm/proc/
 * threads init has completed, so secondaries' GIC/atomic activity
 * doesn't race primary's setup.
 *
 * Without this flag, NUM_CPUS=4 was observed to hang at "vm: init
 * done" when secondaries wrote BSS markers in _other_core_virtual
 * (cache-line contention with primary's vm subsystem). With the
 * flag, secondaries park at the assembly-side wait until primary
 * publishes readiness, then complete their bring-up in parallel
 * with primary's idle loop / scheduler. */
volatile unsigned int hal_smpPrimaryReady;


void _hal_interruptsInitPerCPU(void)
{
	unsigned int timerIrq;
	unsigned int myCpuId = hal_cpuGetID();

	if (myCpuId < (sizeof(hal_smpInterruptsInitPerCpuCount) / sizeof(hal_smpInterruptsInitPerCpuCount[0]))) {
		hal_cpuAtomicInc(&hal_smpInterruptsInitPerCpuCount[myCpuId]);
	}

	*(interrupts_common.gicc + gicc_ctlr) &= ~0x3U;

	/* Initialize CPU Interface of the gic
	 * Set the maximum priority mask and binary point */
	*(interrupts_common.gicc + gicc_bpr) = 3;
	*(interrupts_common.gicc + gicc_pmr) = 0xff;

	/* EnableGrp0 = 1; EnableGrp1 = 1; AckCtl = 1; FIQEn = 1 in secure mode
	 * EnableGrp1 = 1 in non-secure mode, other bits are ignored */
	*(interrupts_common.gicc + gicc_ctlr) = *(interrupts_common.gicc + gicc_ctlr) | 0xfU;

	/* SMP Phase C step 2 (see docs/notes/2026-05-22-smp-phase-c-design.md):
	 * the architectural timer is a PPI, and PPI enable bits in
	 * GICD_ISENABLER0 (offset 0x100) are banked per-CPU. Primary's
	 * hal_interruptsSetHandler() at boot enables the bit for CPU 0;
	 * each secondary CPU must enable the same PPI for its own banked
	 * register. Without this the secondary's gtimer fires but the
	 * GIC drops the interrupt and the secondary stays in WFI forever.
	 *
	 * On secondaries we spin briefly until primary's _hal_timerInit
	 * has set timer_common.ready (so hal_timerIrq() returns the real
	 * PPI number). The window is small (primary runs _hal_cpuInit →
	 * SEV → _hal_timerInit back-to-back). Primary itself doesn't
	 * need to do anything here — its later hal_interruptsSetHandler
	 * for the timer enables its own banked bit. */
	if (myCpuId != 0U) {
		do {
			timerIrq = hal_timerIrq();
			if (timerIrq != 0U) {
				break;
			}
			__asm__ volatile ("yield" ::: "memory");
		} while (1);
		if (timerIrq < SPI_FIRST_IRQID) {
			interrupts_enableIRQ(timerIrq);
			if (myCpuId < (sizeof(hal_smpInterruptsEnabledPpiCount) / sizeof(hal_smpInterruptsEnabledPpiCount[0]))) {
				hal_cpuAtomicInc(&hal_smpInterruptsEnabledPpiCount[myCpuId]);
			}
		}
	}
}


static void hal_cpuSendSGI(u8 targetFilter, u8 targetList, u8 intID)
{
	*(interrupts_common.gicd + gicd_sgir) = ((u32)(targetFilter & 0x3UL) << 24) | ((u32)targetList << 16) | ((u32)intID & 0xfU);
	hal_cpuDataSyncBarrier();
}


void hal_cpuSendIPI(unsigned int cpu, unsigned int intr)
{
	if ((cpu >= hal_cpuGetCount()) || (cpu >= 8U)) {
		return;
	}

	hal_cpuSendSGI(SGI_FLT_USE_LIST, (u8)(1U << cpu), (u8)intr);
}


void hal_cpuBroadcastIPI(unsigned int intr)
{
	hal_cpuSendSGI(SGI_FLT_OTHER_CPUS, 0, (u8)intr);
}
