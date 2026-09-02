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
	void *block;

	if (zone == NULL) {
		return NULL;
	}

	if (zone->used == zone->blocks) {
		return NULL;
	}

	block = zone->first;
#if VM_ZONE_POISON
	zone_checkPoison(zone, block);
#endif
	zone->first = *((void **)(zone->first));
	zone->used++;

	if (addr != NULL) {
		*addr = zone->pages->addr + ((addr_t)block - (addr_t)zone->vaddr);
	}

	return block;
}


void _vm_zfree(vm_zone_t *zone, void *block)
{
	if (((ptr_t)block < (ptr_t)zone->vaddr) || ((ptr_t)block >= (ptr_t)zone->vaddr + zone->blocksz * zone->blocks)) {
		return;
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
		return;
	}

#if VM_ZONE_POISON
	zone_poison(zone, block, __builtin_return_address(0));
#endif

	*((void **)block) = zone->first;
	zone->first = block;
	zone->used--;

	return;
}


void _zone_init(vm_map_t *map, vm_object_t *kernel, void **bss, void **top)
{
	zone_common.kmap = map;
	zone_common.kernel = kernel;
}
