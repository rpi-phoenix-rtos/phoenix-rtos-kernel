/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Virtual memory manager - page allocator
 *
 * Copyright 2012, 2016 Phoenix Systems
 * Copyright 2001, 2005 Pawel Pisarczyk
 * Author: Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_VM_PAGE_H_
#define _PH_VM_PAGE_H_

#include "hal/hal.h"
#include "include/sysinfo.h"
#include "types.h"


page_t *vm_pageAlloc(size_t size, vm_flags_t flags);


void vm_pageFree(page_t *p);


/* returns NULL when addr is outside of defined physical maps (MMU) */
page_t *page_get(addr_t addr);


void _page_showPages(void);


int page_map(pmap_t *pmap, void *vaddr, addr_t pa, vm_attr_t attr);


int _page_sbrk(pmap_t *pmap, void **start, void **end);


void vm_pageGetStats(size_t *freesz);


void vm_pageinfo(meminfo_t *info);


void _page_init(pmap_t *pmap, void **bss, void **top);


/*
 * TODO(C1-hunt): physical-page provenance log, compiled in only with -DC1_PAGE_PROVENANCE.
 *
 * Records who allocated, freed or physically mapped each page of a fixed physical band, so
 * that the userspace C1 detector can ask, at the moment it finds a corrupted page, who held
 * that page before malloc. See vm/page.c for the log and docs/c1-heap-corruption.md for the
 * question it answers. Implemented for MMU targets only (not vm/page-nommu.c).
 *
 * vm_pageAlloc() only knows the owner flags, which cannot tell an anonymous page from a
 * page-cache page or a MAP_CONTIGUOUS block, so the call sites that can tell say so through
 * VM_PAGE_ALLOC(). Without the define it expands to exactly vm_pageAlloc(size, flags) and
 * the kind argument is not evaluated.
 */
#define PAGE_PROV_NONE   0U /* derive the kind from the owner flags */
#define PAGE_PROV_ANON   1U /* anonymous page of a user map */
#define PAGE_PROV_KANON  2U /* anonymous page of the kernel map (large vm_kmalloc) */
#define PAGE_PROV_COW    3U /* copy-on-write copy of an object or shared anonymous page */
#define PAGE_PROV_FILE   4U /* page-cache page of a file object */
#define PAGE_PROV_CONTIG 5U /* MAP_CONTIGUOUS block */
#define PAGE_PROV_MSG    6U /* kernel copy of a message's unaligned head or tail */

#ifdef C1_PAGE_PROVENANCE

/*
 * The dump is requested through meminfo() rather than a new syscall, so that no syscall
 * number or libc stub changes: with page.mapsz, entry.mapsz, entry.kmapsz and maps.mapsz all
 * set to this value, meminfo() prints the log of maps.free pages from physical address
 * maps.total, tagged with reason entry.pid, and returns the number of lines printed in
 * page.alloc. A kernel built without the define rejects every one of those sizes (< -1)
 * and writes nothing, so the caller sees page.alloc unchanged. Keep in sync with
 * libphoenix stdlib/malloc_dl.c.
 */
#define C1PROV_MEMINFO_MAGIC (-0xc1)

page_t *vm_pageAllocProv(size_t size, vm_flags_t flags, unsigned int kind);


/* Records a user MAP_PHYSMEM mapping of [pa, pa + size) */
void vm_pageProvPhys(addr_t pa, size_t size, vm_flags_t flags);


/* Prints the log of up to 4 pages from pa on the kernel log; returns the number of lines */
unsigned int vm_pageProvDump(addr_t pa, size_t npages, unsigned int reason);


#define VM_PAGE_ALLOC(size, flags, kind) vm_pageAllocProv((size), (flags), (kind))

#else

#define VM_PAGE_ALLOC(size, flags, kind) vm_pageAlloc((size), (flags))

#endif


#endif
