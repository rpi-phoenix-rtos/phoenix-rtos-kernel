/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Virtual memory manager - zone allocator
 *
 * Copyright 2014, 2016-2017 Phoenix Systems
 * Author: Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include "hal/hal.h"
#include "lib/lib.h"
#include "include/errno.h"
#include "page.h"
#include "map.h"
#include "zone.h"


/* ---- free-block poisoning ------------------------------------------------
 *
 * OFF by default (zero cost); build the kernel with -DVM_ZONE_POISON=1.
 *
 * A freed block keeps the free-list link in its first word, so a write into
 * already-freed memory silently replaces that link and the allocator only falls
 * over at some later, unrelated allocation -- the crash is then reported
 * against an innocent caller. That is exactly the failure recorded in
 * docs/misc/2026-09-02-kernel-heap-corruption-workorder.md, where _vm_zalloc
 * faulted following a link that had become the ASCII path "/test_st".
 *
 * With poisoning on, a free stamps the rest of the block with a known pattern
 * and records who freed it; the next allocation of that block verifies the
 * pattern. A write-after-free is then reported at the very next allocation of
 * that block, naming the freeing site and dumping the bytes that replaced the
 * poison -- which is usually enough to recognise the writer (a path string, a
 * struct, a length).
 */
#ifndef VM_ZONE_POISON
#define VM_ZONE_POISON 0
#endif

#if VM_ZONE_POISON

#define ZONE_POISON_BYTE 0xa5u
/* word 0 is the free-list link, word 1 records the freeing site */
#define ZONE_POISON_OFFS (2 * sizeof(void *))

static void zone_poison(vm_zone_t *zone, void *block, void *freer)
{
	if (zone->blocksz <= ZONE_POISON_OFFS) {
		return;
	}
	((void **)block)[1] = freer;
	hal_memset((char *)block + ZONE_POISON_OFFS, (int)ZONE_POISON_BYTE,
		zone->blocksz - ZONE_POISON_OFFS);
}


static void zone_checkPoison(vm_zone_t *zone, void *block)
{
	const unsigned char *p = (const unsigned char *)block;
	size_t i;

	if (zone->blocksz <= ZONE_POISON_OFFS) {
		return;
	}
	for (i = ZONE_POISON_OFFS; i < zone->blocksz; ++i) {
		if (p[i] != (unsigned char)ZONE_POISON_BYTE) {
			lib_printf("vm: WRITE-AFTER-FREE block=%p zone_blocksz=%u freed_by=%p "
				"first_bad_offs=%u bytes=", block, (unsigned int)zone->blocksz,
				((void **)block)[1], (unsigned int)i);
			for (i = 0; (i < 16u) && (i < zone->blocksz); ++i) {
				lib_printf("%02x", p[i]);
			}
			lib_printf(" ascii=");
			for (i = 0; (i < 16u) && (i < zone->blocksz); ++i) {
				lib_printf("%c", ((p[i] >= 0x20u) && (p[i] < 0x7fu)) ? (char)p[i] : '.');
			}
			lib_printf("\n");
			return;
		}
	}
}

#endif


/* ---- allocation trace ring ------------------------------------------------
 *
 * OFF by default; build with -DVM_ZONE_TRACE=1.
 *
 * Complements the poisoning above rather than repeating it. Poisoning has to
 * memset every freed block, which measurably shifts allocation timing -- and
 * the corruption we are chasing is a race that stopped reproducing once the
 * memset was in place. This records two words per alloc/free into a ring and
 * touches nothing else, so the timing is barely perturbed.
 *
 * The ring is dumped by the link check in _vm_zalloc, which knows WHICH block
 * was corrupted; the recent history of that block then names the code that
 * owned it (and, if it appears twice as a free, a double free).
 */
#ifndef VM_ZONE_TRACE
#define VM_ZONE_TRACE 0
#endif

#if VM_ZONE_TRACE

#define ZONE_TRACE_ENTRIES 512u

static struct {
	void *block;
	void *caller;
	unsigned int seq;
	unsigned char alloc;
} zone_trace[ZONE_TRACE_ENTRIES];

static unsigned int zone_traceNext;

static void zone_traceAdd(void *block, void *caller, unsigned char alloc)
{
	unsigned int i = zone_traceNext++;

	zone_trace[i % ZONE_TRACE_ENTRIES].block = block;
	zone_trace[i % ZONE_TRACE_ENTRIES].caller = caller;
	zone_trace[i % ZONE_TRACE_ENTRIES].seq = i;
	zone_trace[i % ZONE_TRACE_ENTRIES].alloc = alloc;
}


/* Print every recorded operation on `block` (oldest first), then the tail of
 * the whole ring for context. */
static void zone_traceDump(void *block)
{
	unsigned int i, n = (zone_traceNext < ZONE_TRACE_ENTRIES) ? zone_traceNext : ZONE_TRACE_ENTRIES;
	unsigned int base = zone_traceNext - n;

	lib_printf("vm: trace for block=%p\n", block);
	for (i = 0; i < n; ++i) {
		unsigned int k = (base + i) % ZONE_TRACE_ENTRIES;
		if (zone_trace[k].block == block) {
			lib_printf("vm:   #%u %s caller=%p\n", zone_trace[k].seq,
				(zone_trace[k].alloc != 0u) ? "alloc" : "free ", zone_trace[k].caller);
		}
	}
	lib_printf("vm: last operations (any block)\n");
	for (i = (n > 24u) ? (n - 24u) : 0u; i < n; ++i) {
		unsigned int k = (base + i) % ZONE_TRACE_ENTRIES;
		lib_printf("vm:   #%u %s block=%p caller=%p\n", zone_trace[k].seq,
			(zone_trace[k].alloc != 0u) ? "alloc" : "free ", zone_trace[k].block,
			zone_trace[k].caller);
	}
}

#endif




static struct {
	vm_map_t *kmap;
	vm_object_t *kernel;
} zone_common;


int _vm_zoneCreate(vm_zone_t *zone, size_t blocksz, unsigned int blocks)
{
	size_t i;

	/* blocksz has to be a power of 2 */
	if ((blocksz == 0UL) || (blocks == 0U) || ((blocksz & (blocksz - 1UL)) != 0UL)) {
		return -EINVAL;
	}

	zone->pages = vm_pageAlloc(blocks * blocksz, PAGE_OWNER_KERNEL | PAGE_KERNEL_HEAP);
	if (zone->pages == NULL) {
		return -ENOMEM;
	}

	zone->vaddr = vm_mmap(zone_common.kmap, zone_common.kmap->start, zone->pages, (size_t)1U << zone->pages->idx, PROT_READ | PROT_WRITE, zone_common.kernel, -1, MAP_NONE);
	if (zone->vaddr == NULL) {
		vm_pageFree(zone->pages);
		return -ENOMEM;
	}

	/* Prepare zone for allocations */
#if VM_ZONE_POISON
	/* blocksz is needed by zone_poison() below, which runs inside this loop. */
	zone->blocksz = blocksz;
#endif
	for (i = 0; i < blocks; i++) {
		*((void **)(zone->vaddr + i * blocksz)) = zone->vaddr + (i + 1U) * blocksz;
#if VM_ZONE_POISON
		/* Poison the never-yet-used blocks too. Without this, every block's
		 * FIRST allocation trips the check, because only blocks that have been
		 * through _vm_zfree carry the pattern. */
		zone_poison(zone, zone->vaddr + i * blocksz, NULL);
#endif
	}
	*((void **)(zone->vaddr + ((size_t)blocks - 1U) * blocksz)) = NULL;

	zone->first = zone->vaddr;
	zone->blocks = blocks;
	zone->blocksz = blocksz;
	zone->used = 0;

	return EOK;
}


int _vm_zoneDestroy(vm_zone_t *zone)
{
	if (zone == NULL) {
		return -EINVAL;
	}

	if (zone->used != 0U) {
		return -EBUSY;
	}

	(void)vm_munmap(zone_common.kmap, zone->vaddr, (size_t)1U << zone->pages->idx);
	vm_pageFree(zone->pages);

	zone->vaddr = NULL;
	zone->first = NULL;
	zone->pages = NULL;

	return EOK;
}


void *_vm_zalloc(vm_zone_t *zone, addr_t *addr)
{
	void *block, *next;
	int quarantine = 0;

	if (zone == NULL) {
		return NULL;
	}

	if (zone->used == zone->blocks) {
		return NULL;
	}

	block = zone->first;
	if (block == NULL) {
		/* used < blocks says there should be one. Any accounting drift (or a
		 * quarantine below) would otherwise fault on *(void **)NULL here --
		 * the very failure the link check exists to turn into a diagnostic. */
		return NULL;
	}

#if VM_ZONE_POISON
	zone_checkPoison(zone, block);
#endif

	/* Validate the free-list link BEFORE it becomes zone->first, so a block
	 * whose link word was overwritten while free is reported here -- naming the
	 * corrupted block -- instead of faulting the NEXT allocation, which only
	 * ever saw the garbage and could blame an innocent caller. That is the
	 * failure in docs/misc/2026-09-02-kernel-heap-corruption-workorder.md: the
	 * abort took far = ASCII "/test_st", a path string sitting where a link
	 * belonged, with no way to tell which block had held it.
	 *
	 * Cost is two compares and an AND (blocksz is a power of 2) on the alloc
	 * path. On detection the zone is QUARANTINED rather than followed: the list
	 * is truncated AND used is forced to blocks, so the guard above rejects
	 * every further allocation from it. Truncating alone would not do -- it
	 * leaves first == NULL with used < blocks, so the very next call would pass
	 * that guard and fault on *(void **)NULL, possibly before the diagnostic
	 * had finished reaching the console. Forcing used also makes
	 * _kmalloc_alloc retire the zone to its used list on this same call, and a
	 * later _vm_zfree into it decrements used and relinks the freed block as a
	 * valid one-element list, so the zone rejoins rotation consistently. */
	next = *((void **)block);
	if ((next != NULL) &&
			(((ptr_t)next < (ptr_t)zone->vaddr) ||
					((ptr_t)next >= (ptr_t)zone->vaddr + zone->blocksz * zone->blocks) ||
					((((ptr_t)next - (ptr_t)zone->vaddr) & (zone->blocksz - 1UL)) != 0UL))) {
		lib_printf("vm: CORRUPT free-list link block=%p link=%p zone=[%p..%p) blocksz=%u used=%u\n",
			block, next, zone->vaddr, (char *)zone->vaddr + zone->blocksz * zone->blocks,
			(unsigned int)zone->blocksz, zone->used);
#if VM_ZONE_TRACE
		zone_traceDump(block);
#endif
		next = NULL;
		quarantine = 1;
	}

	zone->first = next;
	zone->used++;
	if (quarantine != 0) {
		zone->used = zone->blocks;
	}
#if VM_ZONE_TRACE
	zone_traceAdd(block, __builtin_return_address(0), 1u);
#endif

	if (addr != NULL) {
		*addr = zone->pages->addr + ((addr_t)block - (addr_t)zone->vaddr);
	}

	return block;
}


int _vm_zfree(vm_zone_t *zone, void *block)
{
	if (((ptr_t)block < (ptr_t)zone->vaddr) || ((ptr_t)block >= (ptr_t)zone->vaddr + zone->blocksz * zone->blocks)) {
		return -EINVAL;
	}

	/* Reject a pointer that is inside the zone but not at a block boundary. The
	 * free list is threaded through the blocks themselves, so linking a
	 * misaligned "block" writes the list head into the middle of a neighbour and
	 * hands that address out later -- silent heap corruption that only surfaces
	 * at some unrelated allocation. Refusing costs one modulo on the free path
	 * and turns a mis-computed or interior pointer into a leak instead.
	 *
	 * Range was already checked above; this closes the other half. See
	 * docs/misc/2026-09-02-kernel-heap-corruption-workorder.md for the crash
	 * that motivated it (a free block whose link had been replaced by a path
	 * string, faulting the next _vm_zalloc). */
	if ((((ptr_t)block - (ptr_t)zone->vaddr) % zone->blocksz) != 0) {
		return -EINVAL;
	}

#if VM_ZONE_POISON
	zone_poison(zone, block, __builtin_return_address(0));
#endif
#if VM_ZONE_TRACE
	zone_traceAdd(block, __builtin_return_address(0), 0u);
#endif

	*((void **)block) = zone->first;
	zone->first = block;
	zone->used--;

	return EOK;
}


void _zone_init(vm_map_t *map, vm_object_t *kernel, void **bss, void **top)
{
	zone_common.kmap = map;
	zone_common.kernel = kernel;
}
