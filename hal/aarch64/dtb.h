/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * DTB parser
 *
 * Copyright 2018, 2024 Phoenix Systems
 * Author: Pawel Pisarczyk, Lukasz Leczkowski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_DTB_H_
#define _PH_HAL_DTB_H_

#include "hal/types.h"

#define ntoh16(x) ((u16)(((u16)(x) << 8) & 0xff00U) | (((x) >> 8) & 0xffU))
#define ntoh32(x) ((u32)(((u32)ntoh16(x) << 16) | ((u32)ntoh16((x) >> 16))))
#define ntoh64(x) ((u64)(((u64)ntoh32(x) << 32) | ((u64)ntoh32((x) >> 32))))

typedef struct {
	addr_t start;
	addr_t end;
} dtb_memBank_t;


/* TD-15 / Stage 2 phase 4: reserved-memory region.
 * Populated from /reserved-memory child nodes' reg properties. The kernel
 * page-frame allocator must skip these regions; firmware/VC4 uses them. */
typedef struct {
	addr_t start;
	addr_t end;
	const char *name; /* node name for diagnostics; points into the live DTB */
} dtb_resvMemRegion_t;


/* TD-15 / Stage 2 phase 4: ARM↔BUS DMA address translation entry.
 * Populated from /soc/dma-ranges. dtb_armToBus(arm_pa) maps a CPU PA into
 * the bus PA that peripherals see (Pi 4: legacy 0xc0000000 alias and the
 * full-RAM 0x0 alias). */
typedef struct {
	addr_t bus;
	addr_t cpu;
	addr_t size;
} dtb_dmaRange_t;


typedef struct {
	addr_t base;
	int intr;
} dtb_serial_t;


typedef struct {
	int physSecure;
	int physNonSecure;
	int virt;
	int hyp;
} dtb_timer_t;


typedef enum {
	dtb_timerNone = 0,
	dtb_timerPhysNonSecure,
	dtb_timerVirt,
} dtb_timerSource_t;


void dtb_getSystem(char **model, char **compatible);


int dtb_getCPU(unsigned int n, char **compatible, u32 *clock);


void dtb_getMemory(dtb_memBank_t **banks, size_t *nBanks);


/* TD-15 / Stage 2 phase 4. */
void dtb_getReservedMemory(dtb_resvMemRegion_t **regions, size_t *nRegions);


/* TD-15 / Stage 2 phase 4: ARM↔BUS DMA range table from /soc/dma-ranges. */
void dtb_getDmaRanges(dtb_dmaRange_t **ranges, size_t *nRanges);


/* TD-15 / Stage 2 phase 4: translate an ARM CPU PA into the bus PA a
 * peripheral DMA descriptor must use. Returns EOK on success and writes the
 * translated address into *busAddr; returns -ENODEV when no /soc/dma-ranges
 * entry covers cpuAddr. */
int dtb_armToBus(addr_t cpuAddr, addr_t *busAddr);


void dtb_getGIC(addr_t *gicc, addr_t *gicd);


void dtb_getSerials(dtb_serial_t **serials, size_t *nSerials);


int dtb_getConsoleSerial(dtb_serial_t *serial);


void dtb_getTimer(dtb_timer_t *timer);


int dtb_getTimerSource(dtb_timerSource_t *source, int *intr);


void _dtb_init(addr_t dtbPhys);


#endif
