/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Virtual memory manager - memory object abstraction
 *
 * Copyright 2016-2017 Phoenix Systems
 * Author: Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PH_VM_OBJECT_H_
#define _PH_VM_OBJECT_H_

#include "hal/hal.h"
#include "lib/lib.h"
#include "proc/lock.h"
#include "amap.h"


struct _vm_map_t;

typedef struct _vm_object_t {
	rbnode_t linkage;
	oid_t oid;
	int refs;
	u8 flags;           /* VM_OBJ_* below */
	vm_flags_t memtype; /* export window: MAP_UNCACHED/MAP_DEVICE bits every mapping must match */
	size_t size;

	/* Memory export (vm_objectExport); NULL for any other object */
	struct _vm_object_t *parent;  /* object owning the pages of an export window */
	struct _vm_object_t *next;    /* list of published exports */
	struct _vm_object_t *prev;

	page_t *pages[];
} vm_object_t;


#define VM_OBJ_PHYSMEM ((vm_object_t *)-1)


/* vm_object_t.flags */
#define VM_OBJ_EXPORT    (1U << 0) /* export window: pages borrowed from parent, memtype enforced */
#define VM_OBJ_PUBLISHED (1U << 1) /* export window reachable by oid (in the object tree) */
#define VM_OBJ_SHARED    (1U << 2) /* pages are exported: mappings are shared, not COW, across fork */


vm_object_t *vm_objectRef(vm_object_t *o);


int vm_objectGet(vm_object_t **o, oid_t oid);


int vm_objectPut(vm_object_t *o);


/*
 * Fills appropriate page_t struct into *res, allocates new page when o == NULL.
 * Note that when o == VM_OBJ_PHYSMEM and offs is outside of defined physical maps,
 * then *res will be NULL even though mapping can still be made.
 */
int vm_objectPage(struct _vm_map_t *map, amap_t **amap, vm_object_t *o, void *vaddr, u64 offs, page_t **page);


vm_object_t *vm_objectContiguous(size_t size);


/*
 * Publishes the page-aligned range [vaddr, vaddr + size) of a MAP_CONTIGUOUS mapping in map
 * as a new object under oid, so that mmap() of a descriptor carrying that oid maps the same
 * pages. The memory type of the source mapping is fixed for the export. The caller must have
 * checked that oid.port belongs to the calling process.
 */
int vm_objectExport(struct _vm_map_t *map, oid_t oid, void *vaddr, size_t size);


/* Withdraws the export published under oid; existing mappings keep the pages. */
int vm_objectUnexport(oid_t oid);


/* Withdraws every export published under port. Called when the port id is released. */
void vm_objectUnexportPort(u32 port);


/* Checks that a mapping of [offs, offs + size) of o with flags is allowed (export windows). */
int vm_objectMapCheck(const vm_object_t *o, u64 offs, size_t size, vm_flags_t flags);


/* Returns nonzero if mappings of o must stay shared across fork. */
int vm_objectShared(const vm_object_t *o);


int _object_init(struct _vm_map_t *kmap, vm_object_t *kernel);


#endif
