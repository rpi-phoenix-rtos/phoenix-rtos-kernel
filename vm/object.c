/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Virtual memory manager - object management
 *
 * Copyright 2017, 2020 Phoenix Systems
 * Author: Pawel Pisarczyk, Jan Sikorski, Maciej Purski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "include/errno.h"
#include "include/file.h"
#include "lib/lib.h"
#include "page.h"
#include "kmalloc.h"
#include "object.h"
#include "map.h"
#include "proc/proc.h"
#include "proc/name.h"
#include "proc/threads.h"


/*
 * File object cache
 *
 * Without it, the pages of a file object are freed with its last reference, so every exec of a
 * program and every mmap() of a file that nothing maps at the moment reads it from the server
 * again (on the NFS root, one round trip of ~3 ms per 16-page read-ahead cluster: ~7.5 s per start
 * of a 330 MB browser). With it, an unreferenced file object keeps its pages, stays in the tree and
 * goes on an LRU list; vm_objectGet() of its oid reuses it.
 *
 * What is cached: an object of a regular file (the server answers mtGetAttr(atType) with otFile)
 * whose size, mtime and ctime the server reports (mtGetAttrAll), and whose pages were never
 * writable in memory: a mapping with PROT_WRITE without MAP_NEEDSCOPY (any writable mmap() of a
 * file -- MAP_SHARED and MAP_PRIVATE are the same here) or an mprotect() to PROT_WRITE writes the
 * object's pages, which are never written back; today such changes vanish with the last mapping,
 * and they must not outlive it in the cache (vm_objectWritable()). Never cached: export windows,
 * contiguous objects, anonymous memory, the kernel object.
 *
 * Staleness. A cached object is reused only if it still describes the file:
 *  - Changes the kernel passes on: proc_send() reports every mtWrite, mtTruncate, mtSetAttr,
 *    mtDestroy and mtUnlink of an oid, and the oid of every mtCreate response (a server may give
 *    a new file the id of a removed one -- dummyfs and ext2 do), to vm_objectNotify(). A cached
 *    object of that oid is freed at once; a referenced one is marked stale, so it is freed rather
 *    than cached when its last reference goes (and after mtCreate it also leaves the tree, so the
 *    new file never shares the old file's pages). Every write(), truncate and unlink of a file on
 *    this system goes this way.
 *  - Changes nobody tells the kernel about (another NFS client, e.g. the build host overwriting a
 *    binary on the export): before an unreferenced object is reused, the server is asked for size,
 *    mtime and ctime again (one mtGetAttrAll) and the object is reused only if all three are the
 *    ones it was created with. The times are in seconds: a rewrite by another client to the same
 *    size within the second the object was created in is not seen, nor one younger than the NFS
 *    server's attribute cache (100 ms). Such a rewrite is not seen by a referenced object today
 *    either.
 *  - A server that goes away: its cached objects are freed when its port is released.
 *
 * Memory. Cached pages are free memory as far as anybody else is concerned: vm_pageAlloc() and
 * page_map() call vm_objectReclaim() when the allocator has nothing left and retry, so a cached
 * page never causes an allocation failure; meminfo reports them as free. Memory that is really
 * free is kept above 1/VM_OBJCACHE_LOWWATER of RAM: below it every allocation evicts the least
 * recently used object (vm_objectReclaimLow()). That matters for contiguous blocks (kmalloc zones,
 * amap arrays, MAP_CONTIGUOUS): pages given back only when nothing else is left lie scattered
 * between the pages taken meanwhile. (Build 43, without this: a process using up all memory died
 * at the first touch of a new mapping -- its amap array wants a 256 KB block -- 576 MB earlier
 * than on a kernel without the cache.) The cache is also capped (VM_OBJCACHE_PERCENT).
 *
 * Locking. object_common.lock is taken under a map's lock (fault path, vm_objectWritable()) and
 * under kmalloc_common.lock (a zone created by vm_kmalloc() allocates pages: vm_pageAlloc() ->
 * vm_objectReclaim()); it is held only around list and tree changes, and nothing allocates, sends
 * a message or takes a map, amap or kmalloc lock under it. Pages are freed (pages_info.lock) with
 * object_common.lock held or not; pages_info.lock is never held while object_common.lock is taken
 * (the allocator drops it before reclaiming). vm_objectReclaim() never frees kernel heap -- it can
 * run under kmalloc_common.lock -- so the header of an object it evicts goes on a list that
 * vm_objectGet() and vm_objectPut() free (object_reap()).
 */

#ifndef VM_OBJCACHE
#define VM_OBJCACHE 0
#endif

/* Upper bound of the pages kept by unreferenced objects, in percent of the memory free at boot */
#ifndef VM_OBJCACHE_PERCENT
#define VM_OBJCACHE_PERCENT 25U
#endif

/* Free memory (not counting the cache) is kept above 1/VM_OBJCACHE_LOWWATER of the memory free
 * at boot: below it, every allocation evicts an object, and no object is kept */
#ifndef VM_OBJCACHE_LOWWATER
#define VM_OBJCACHE_LOWWATER 16U
#endif

/* otFile of <sys/file.h> */
#define OBJECT_OTFILE 1


static struct {
	rbtree_t tree;
	vm_object_t *kernel;
	vm_map_t *kmap;
	vm_object_t *exports; /* published export windows, see vm_objectExport() */
	lock_t lock;

	/* File object cache */
	vm_object_t *lru;   /* unreferenced cached objects, least recently released first */
	vm_object_t *reaped; /* evicted by vm_objectReclaim(): pages freed, header not yet (via next) */
	size_t cached;      /* pages held by the objects on lru */
	size_t cacheMax;    /* bound of cached, in pages */
	size_t lowWater;    /* free memory below which nothing is kept, in pages */
} object_common;


/* What a server says about a file, for the cache */
typedef struct {
	int valid; /* a regular file whose size and times the server reported */
	off_t size;
	long long mtime;
	long long ctime;
} object_attrs_t;


/* An anonymous contiguous object (vm_objectContiguous) carries this oid and is never in the tree */
static int object_isContiguous(const vm_object_t *o)
{
	return ((o->oid.port == (u32)(-1)) && (o->oid.id == (id_t)(-1))) ? 1 : 0;
}


/* Makes a published export unreachable by oid. Called with object_common.lock held. */
static void _object_unpublish(vm_object_t *o)
{
	lib_rbRemove(&object_common.tree, &o->linkage);
	LIST_REMOVE(&object_common.exports, o);
	o->flags &= (u8)~VM_OBJ_PUBLISHED;
}


static int object_cmp(rbnode_t *n1, rbnode_t *n2)
{
	vm_object_t *o1 = lib_treeof(vm_object_t, linkage, n1);
	vm_object_t *o2 = lib_treeof(vm_object_t, linkage, n2);

	/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "Variable pass to lib_treeof will not be NULL, so lib_treeof will not be NULL either" */
	if (o1->oid.id > o2->oid.id) {
		return 1;
	}
	if (o1->oid.id < o2->oid.id) {
		return -1;
	}

	if (o1->oid.port > o2->oid.port) {
		return 1;
	}
	if (o1->oid.port < o2->oid.port) {
		return -1;
	}

	return 0;
}


static vm_object_t *_object_find(oid_t oid)
{
	vm_object_t t;

	hal_memcpy(&t.oid, &oid, sizeof(oid));

	return lib_treeof(vm_object_t, linkage, lib_rbFind(&object_common.tree, &t.linkage));
}


/* Frees the pages of a file object (not of an export window or a contiguous object) */
static void object_freePages(vm_object_t *o)
{
	size_t i;

	for (i = 0; i < round_page(o->size) / SIZE_PAGE; ++i) {
		if (o->pages[i] != NULL) {
			vm_pageFree(o->pages[i]);
			o->pages[i] = NULL;
		}
	}
}


#if VM_OBJCACHE

/*
 * Asks the server about a file: size, mtime and ctime of a regular file, or valid == 0. With
 * probe, it asks the type with mtGetAttr first: every server whose objects can be mapped answers
 * mtGetAttr (vm_objectGet() has always asked it the size), not necessarily mtGetAttrAll.
 */
static void object_attrs(oid_t oid, int probe, object_attrs_t *a)
{
	struct {
		msg_t msg;
		struct _attrAll attrs;
	} *q;
	int err = EOK;

	a->valid = 0;

	q = vm_kmalloc(sizeof(*q));
	if (q == NULL) {
		return;
	}

	if (probe != 0) {
		hal_memset(&q->msg, 0, sizeof(q->msg));
		q->msg.type = mtGetAttr;
		hal_memcpy(&q->msg.oid, &oid, sizeof(oid));
		q->msg.i.attr.type = atType;

		err = proc_send(oid.port, &q->msg);
		if (err == EOK) {
			err = q->msg.o.err;
		}
		if ((err == EOK) && (q->msg.o.attr.val != OBJECT_OTFILE)) {
			err = -EINVAL;
		}
	}

	if (err == EOK) {
		hal_memset(q, 0, sizeof(*q));
		/* A server that leaves an attribute untouched must not report it as 0 */
		q->attrs.type.err = -ENOSYS;
		q->attrs.size.err = -ENOSYS;
		q->attrs.mTime.err = -ENOSYS;
		q->attrs.cTime.err = -ENOSYS;

		q->msg.type = mtGetAttrAll;
		hal_memcpy(&q->msg.oid, &oid, sizeof(oid));
		q->msg.o.data = &q->attrs;
		q->msg.o.size = sizeof(q->attrs);

		err = proc_send(oid.port, &q->msg);
		if (err == EOK) {
			err = q->msg.o.err;
		}
		if ((err == EOK) && (q->attrs.type.err == EOK) && (q->attrs.type.val == OBJECT_OTFILE) &&
				(q->attrs.size.err == EOK) && (q->attrs.size.val >= 0) &&
				(q->attrs.mTime.err == EOK) && (q->attrs.cTime.err == EOK)) {
			a->valid = 1;
			a->size = (off_t)q->attrs.size.val;
			a->mtime = q->attrs.mTime.val;
			a->ctime = q->attrs.cTime.val;
		}
	}

	vm_kfree(q);
}


/* Takes a cached object off the LRU, to be used again. Called with object_common.lock held. */
static void _object_uncache(vm_object_t *o)
{
	LIST_REMOVE(&object_common.lru, o);
	object_common.cached -= o->resident;
	o->flags &= (u8)~VM_OBJ_CACHED;
	o->next = NULL;
	o->prev = NULL;
}


/* Takes a cached object off the LRU and out of the tree, to be freed. Called with
 * object_common.lock held. */
static void _object_evict(vm_object_t *o)
{
	_object_uncache(o);
	lib_rbRemove(&object_common.tree, &o->linkage);
}


/* Frees the objects of a list made by _object_trim() */
static void object_freeList(vm_object_t *o)
{
	vm_object_t *next;

	while (o != NULL) {
		next = o->next;
		object_freePages(o);
		vm_kfree(o);
		o = next;
	}
}


/* Evicts objects until the cache is within its bounds; returns them as a list to free once the
 * lock is dropped. Called with object_common.lock held. */
static vm_object_t *_object_trim(void)
{
	vm_object_t *o, *list = NULL;
	size_t freesz, need = 0, got = 0;

	/* Free memory moves only when the list is freed: evict what lifts it to the low watermark */
	vm_pageGetStats(&freesz);
	freesz /= SIZE_PAGE;
	if (freesz < object_common.lowWater) {
		need = object_common.lowWater - freesz;
	}

	while ((object_common.lru != NULL) && ((object_common.cached > object_common.cacheMax) || (got < need))) {
		o = object_common.lru;
		_object_evict(o);
		got += o->resident;
		o->next = list;
		list = o;
	}

	return list;
}


/* Keeps an object whose last reference was dropped. Returns nonzero if it is now cached. Called
 * with object_common.lock held. */
static int _object_cachePut(vm_object_t *o)
{
	size_t i, n = 0;

	if ((o->flags & (VM_OBJ_CACHEABLE | VM_OBJ_STALE | VM_OBJ_UNLINKED)) != VM_OBJ_CACHEABLE) {
		return 0;
	}

	for (i = 0; i < round_page(o->size) / SIZE_PAGE; ++i) {
		if (o->pages[i] != NULL) {
			n++;
		}
	}

	if ((n == 0U) || (n > object_common.cacheMax)) {
		return 0;
	}

	o->resident = n;
	o->flags |= (u8)VM_OBJ_CACHED;
	LIST_ADD(&object_common.lru, o);
	object_common.cached += n;

	return 1;
}


/* Frees the headers of objects evicted by vm_objectReclaim(). Not called under any lock. */
static void object_reap(void)
{
	vm_object_t *o, *next;

	if (object_common.reaped == NULL) {
		return;
	}

	(void)proc_lockSet(&object_common.lock);
	o = object_common.reaped;
	object_common.reaped = NULL;
	(void)proc_lockClear(&object_common.lock);

	while (o != NULL) {
		next = o->next;
		vm_kfree(o);
		o = next;
	}
}

#endif /* VM_OBJCACHE */


int vm_objectGet(vm_object_t **o, oid_t oid)
{
	vm_object_t *no = NULL;
	object_attrs_t a;
	size_t i, n = 0;
	off_t sz = 0;
	int err = -ENOMEM;
#if VM_OBJCACHE
	vm_object_t *old = NULL; /* stale cached objects to free, via next */
	int asked = 0;
#endif

	hal_memset(&a, 0, sizeof(a));

#if VM_OBJCACHE
	object_reap();
#endif

	(void)proc_lockSet(&object_common.lock);

	for (;;) {
		*o = _object_find(oid);

#if VM_OBJCACHE
		if ((*o != NULL) && (((*o)->flags & VM_OBJ_CACHED) != 0U)) {
			if (asked == 0) {
				/* Ask the server whether the cached pages are still the file's, then look again:
				 * meanwhile the object may have been evicted, reused or found stale */
				(void)proc_lockClear(&object_common.lock);
				object_attrs(oid, 0, &a);
				asked = 1;
				(void)proc_lockSet(&object_common.lock);
				continue;
			}

			if ((a.valid != 0) && (a.size == (off_t)(*o)->size) && (a.mtime == (*o)->mtime) && (a.ctime == (*o)->ctime)) {
				/* Still the file it was cached from: reuse it */
				_object_uncache(*o);
				break;
			}

			/* The file changed (or the server could not say): start afresh */
			_object_evict(*o);
			(*o)->next = old;
			old = *o;
			*o = NULL;
		}
#endif

		if (*o != NULL) {
			break;
		}

		if (no != NULL) {
			*o = no;
			no = NULL;
			hal_memcpy(&(*o)->oid, &oid, sizeof(oid));

			/* Safe to cast - sz fits into size_t from above checks */
			(*o)->size = (size_t)sz;
			(*o)->refs = 0;
			(*o)->flags = 0U;
			(*o)->memtype = 0U;
			(*o)->parent = NULL;
			(*o)->next = NULL;
			(*o)->prev = NULL;
			(*o)->mtime = a.mtime;
			(*o)->ctime = a.ctime;
			(*o)->resident = 0;

			for (i = 0; i < n; ++i) {
				(*o)->pages[i] = NULL;
			}

#if VM_OBJCACHE
			if ((a.valid != 0) && (a.size == sz) && (oid.port != 0U)) {
				(*o)->flags = (u8)VM_OBJ_CACHEABLE;
			}
#endif

			(void)lib_rbInsert(&object_common.tree, &(*o)->linkage);
			break;
		}

		/* Take off the lock to avoid a deadlock in vm_kmalloc */
		(void)proc_lockClear(&object_common.lock);

#if VM_OBJCACHE
		if (a.valid == 0) {
			object_attrs(oid, 1, &a);
			asked = 1;
		}
		sz = (a.valid != 0) ? a.size : proc_size(oid);
#else
		sz = proc_size(oid);
#endif
		if (sz < 0) {
			err = (int)sz;
		}
		/* parasoft-suppress-next-line MISRAC2012-RULE_14_3 "size_t depends on architecture" */
		else if ((sizeof(off_t) <= sizeof(size_t)) || (sz <= (off_t)((size_t)-1))) {
			n = round_page((size_t)sz) / SIZE_PAGE;
			no = (vm_object_t *)vm_kmalloc(sizeof(vm_object_t) + n * sizeof(page_t *));
		}
		else {
			/* No action required */
		}

		if (no == NULL) {
#if VM_OBJCACHE
			object_freeList(old);
#endif
			return err;
		}

		/* Check again, somebody could've added the object in the meantime */
		(void)proc_lockSet(&object_common.lock);
	}

	(*o)->refs++;
	(void)proc_lockClear(&object_common.lock);

	/* Stale cached objects, and one we allocated and didn't need in the end */
#if VM_OBJCACHE
	object_freeList(old);
#endif
	if (no != NULL) {
		vm_kfree(no);
	}

	return EOK;
}


vm_object_t *vm_objectRef(vm_object_t *o)
{
	if ((o != NULL) && (o != VM_OBJ_PHYSMEM)) {
		(void)proc_lockSet(&object_common.lock);
		o->refs++;
		(void)proc_lockClear(&object_common.lock);
	}

	return o;
}


int vm_objectPut(vm_object_t *o)
{
#if VM_OBJCACHE
	vm_object_t *evicted;
#endif

	if ((o == NULL) || (o == VM_OBJ_PHYSMEM)) {
		return EOK;
	}

	(void)proc_lockSet(&object_common.lock);

	if (--o->refs != 0) {
		(void)proc_lockClear(&object_common.lock);
		return EOK;
	}

	/* Only remove what is in the tree: a contiguous object is never inserted (see the fix in
	 * d0fb0ca9 -- rb-removing its zeroed node empties the tree), and an export window leaves
	 * the tree when its export is withdrawn (_object_unpublish). */
	if ((o->flags & VM_OBJ_EXPORT) != 0U) {
		if ((o->flags & VM_OBJ_PUBLISHED) != 0U) {
			_object_unpublish(o);
		}
	}
	else if (object_isContiguous(o) == 0) {
#if VM_OBJCACHE
		if (_object_cachePut(o) != 0) {
			/* Kept with its pages; make room for it if needed */
			evicted = _object_trim();
			(void)proc_lockClear(&object_common.lock);
			object_freeList(evicted);
			object_reap();
			return EOK;
		}
#endif
		if ((o->flags & VM_OBJ_UNLINKED) == 0U) {
			lib_rbRemove(&object_common.tree, &o->linkage);
		}
	}
	else {
		/* No action required */
	}
	(void)proc_lockClear(&object_common.lock);

	if (o->parent != NULL) {
		/* Export window: the pages belong to the parent */
		(void)vm_objectPut(o->parent);
	}
	/* Contiguous object 'holds' all pages in pages[0] */
	else if (object_isContiguous(o) != 0) {
		vm_pageFree(o->pages[0]);
	}
	else {
		object_freePages(o);
	}

	vm_kfree(o);

	return EOK;
}


void vm_objectWritable(vm_object_t *o)
{
#if VM_OBJCACHE
	/* VM_OBJ_CACHEABLE is set before an object enters the tree and only ever cleared after */
	if ((o != NULL) && (o != VM_OBJ_PHYSMEM) && ((o->flags & VM_OBJ_CACHEABLE) != 0U)) {
		(void)proc_lockSet(&object_common.lock);
		o->flags &= (u8)~VM_OBJ_CACHEABLE;
		(void)proc_lockClear(&object_common.lock);
	}
#else
	(void)o;
#endif
}


void vm_objectNotify(const msg_t *msg, int responded)
{
#if VM_OBJCACHE
	vm_object_t *o, *victim = NULL;
	const oid_t *target;

	switch (msg->type) {
		case mtWrite:
		case mtTruncate:
		case mtSetAttr:
		case mtDestroy:
			target = &msg->oid;
			break;

		case mtUnlink:
			/* A name of the file is gone, and maybe the file: the pages are worth nothing more */
			target = &msg->i.ln.oid;
			break;

		case mtCreate:
			target = (responded != 0) ? &msg->o.create.oid : NULL;
			break;

		default:
			return;
	}

	/* Nothing cached and nothing ever was: no lock on the write path */
	if ((target == NULL) || (object_common.cacheMax == 0U)) {
		return;
	}

	(void)proc_lockSet(&object_common.lock);
	o = _object_find(*target);
	if ((o != NULL) && ((o->flags & VM_OBJ_EXPORT) == 0U) && (o != object_common.kernel)) {
		if ((o->flags & VM_OBJ_CACHED) != 0U) {
			_object_evict(o);
			victim = o;
		}
		else {
			o->flags |= (u8)VM_OBJ_STALE;
			if (msg->type == mtCreate) {
				/* The id names a new file now: the next mapping of it must not get these pages */
				lib_rbRemove(&object_common.tree, &o->linkage);
				o->flags |= (u8)VM_OBJ_UNLINKED;
			}
		}
	}
	(void)proc_lockClear(&object_common.lock);

	if (victim != NULL) {
		object_freePages(victim);
		vm_kfree(victim);
	}
#else
	(void)msg;
	(void)responded;
#endif
}


int vm_objectReclaim(void)
{
#if VM_OBJCACHE
	vm_object_t *o;

	/* Nothing cached (also before _object_init()); and never under our own lock, which no
	 * allocation is made under -- this is a guard, not a path */
	if ((object_common.cached == 0U) || (object_common.lock.owner == proc_current())) {
		return 0;
	}

	(void)proc_lockSet(&object_common.lock);
	o = object_common.lru;
	if (o != NULL) {
		_object_evict(o);
	}
	(void)proc_lockClear(&object_common.lock);

	if (o == NULL) {
		return 0;
	}

	object_freePages(o);

	/* The caller may hold kmalloc_common.lock: leave the header to object_reap() */
	(void)proc_lockSet(&object_common.lock);
	o->next = object_common.reaped;
	object_common.reaped = o;
	(void)proc_lockClear(&object_common.lock);

	return 1;
#else
	return 0;
#endif
}


int vm_objectReclaimLow(size_t freesz)
{
#if VM_OBJCACHE
	if ((object_common.cached == 0U) || ((freesz / SIZE_PAGE) >= object_common.lowWater)) {
		return 0;
	}

	return vm_objectReclaim();
#else
	(void)freesz;
	return 0;
#endif
}


size_t vm_objectCachedPages(void)
{
	return object_common.cached;
}


/* Demand-paging read-ahead window. A file-backed page fault previously fetched exactly
 * one 4 KB page and paid THREE synchronous server round-trips for it (proc_open +
 * proc_read + proc_close). Faulting a large binary in one page at a time therefore
 * crawled: a 24 MB static executable spent ~68 s in per-page round-trips before it even
 * reached main() when exec'd from ext2-on-SD. Fetching a bounded cluster per fault with a
 * single open/read/close amortizes that overhead ~an order of magnitude and pre-loads the
 * neighbouring pages sequential code execution is about to touch. The window is bounded
 * (never the whole file) so lazy exec-from-NFS stays lazy (#43). */
#define OBJECT_READAHEAD_PAGES 16u

/* Fetch up to `want` consecutive backing-store pages starting at page-aligned `offs` into
 * freshly-allocated pages (out[0] is the base page for `offs`), using ONE open + one bulk
 * read + one close for the whole cluster. `offs < osize` is guaranteed by the caller; the
 * window is clamped to the object's backing pages so no read is ever issued at/past EOF.
 * Each returned page's tail beyond the file end (or beyond a short read) is zero-filled so
 * stale allocator data can never leak into demand-paged code/data. Returns EOK with
 * *got >= 1 on success, or a negative error with *got == 0 (nothing allocated) on failure.
 * Degrades to a single page under kernel-heap pressure so paging still makes progress. */
static int object_fetchCluster(oid_t oid, u64 offs, size_t osize, size_t want, page_t **out, size_t *got)
{
	page_t *p;
	unsigned int zeroRetry;
	void *buf, *v;
	size_t i, span, total, avail, target;
	int r, err = EOK;

	*got = 0;

	/* Clamp the window to the pages that still back file data (never read past EOF). */
	avail = (size_t)(((u64)round_page(osize) - offs) / SIZE_PAGE);
	if (avail == 0u) {
		avail = 1u;
	}
	if (want > avail) {
		want = avail;
	}
	if (want > OBJECT_READAHEAD_PAGES) {
		want = OBJECT_READAHEAD_PAGES;
	}
	if (want == 0u) {
		want = 1u;
	}

	buf = vm_kmalloc(want * SIZE_PAGE);
	if (buf == NULL) {
		/* Fall back to a single page so demand-paging still progresses under kernel-heap
		 * pressure (this matches the footprint of the old one-page-at-a-time fetch). */
		want = 1u;
		buf = vm_kmalloc(SIZE_PAGE);
		if (buf == NULL) {
			return -ENOMEM;
		}
	}

	/* Real file bytes within the window; the last page may be partial. */
	span = ((u64)osize - offs < (u64)want * SIZE_PAGE) ? (size_t)((u64)osize - offs) : (want * SIZE_PAGE);

	/* NFS exec-over-NFS -EIO fix. This cold cluster open is faulted on the exec demand-page force
	 * path; a single transient proc_open blip here used to fabricate -EIO and abort the whole
	 * ~17 MB exec ("exec ... failed (err=-5)", ~1/10 nfsroot boots). Meanwhile the sibling
	 * proc_read below already tolerates transients (nfs_ops.c retries 25x). Two coupled fixes for
	 * that asymmetry:
	 *   (4a) never fabricate -EIO — propagate proc_open's REAL errno (mirrors the 2026-07-12
	 *        vm_objectPage precedent), so a genuine error stays truthful and diagnosable.
	 *   (4b) bounded backed-off re-drive of THIS one open, matching the read path's resilience —
	 *        NOT a blanket retry bump. Each failed attempt logs the true errno so it is captured
	 *        even when a later retry then succeeds.
	 * Inert on the SD deliverable (SD proc_open does not fail -> loop never entered). */
	r = proc_open(oid, 0);
	if (r < 0) {
		/* The exec's FIRST cold open right after the NFS-root takeover can hit the NFSv4 client's
		 * OPEN-state-establishment window: the mount's GETATTR/FSINFO do not establish OPEN state,
		 * so the first OPEN transiently gets a server "try again" status that libnfs surfaces (via
		 * its catch-all NFS4 mapping, nfs4.c:188) as -ERANGE(-34); it clears within a few seconds
		 * (a manual re-run always succeeds). Observed on HW: the earlier 8-try/~1.9s re-drive was
		 * too short and the exec still aborted with the real -34. Extend to a ~10 s DEADLINE with
		 * ramped backoff: the loop exits the instant the window clears (typically ~2-3 s, not a
		 * fixed wait), so it recovers the exec instead of failing. Bounded and targeted at THIS one
		 * uncovered open (the sibling proc_read already retries in nfs_ops.c) — not a blanket retry.
		 * Each attempt logs the real errno. Inert on SD (proc_open never fails there). */
		const time_t deadline_us = 10000000; /* 10 s cap */
		time_t waited = 0, back = 10000;      /* 10 ms initial backoff, doubled while < 500 ms (so the final step reaches 640 ms), then held */
		int tries = 0;
		while ((r < 0) && (waited < deadline_us)) {
			proc_threadSleep(back);
			waited += back;
			if (back < 500000) {
				back <<= 1;
			}
			tries++;
			r = proc_open(oid, 0);
		}
		/* One summary line for the (rare) recovery, rather than per-iteration spam. */
		lib_printf("object_fetchCluster: NFS OPEN re-drive off=%llu tries=%d waited=%llums rc=%d\n",
			(unsigned long long)offs, tries, (unsigned long long)(waited / 1000), r);
		if (r < 0) {
			vm_kfree(buf);
			return r; /* propagate the REAL error; never fabricate -EIO */
		}
	}

	/* Single bulk read for the whole window, looping over short reads (a backing store
	 * may legitimately satisfy a read with fewer bytes than requested -- normal for NFS
	 * where a READ RPC can return short). Bytes past `total` are zero-filled per page
	 * below, so a short/EOF read never leaves stale data in a mapped page. */
	total = 0;
	zeroRetry = 0u;
	while (total < span) {
		r = proc_read(oid, (off_t)(offs + total), (char *)buf + total, span - total, 0);
		if (r < 0) {
			err = r;
			break;
		}
		if (r == 0) {
			/* EOF before `span`, which is already clamped to the object's own size --
			 * so the store is contradicting what it told us the object contains. The
			 * old code broke straight out and let the per-page zero-fill below cover
			 * the gap, which SILENTLY hands the process a zeroed page where file data
			 * belongs. That is not a theoretical concern: a zeroed .data page is how
			 * libphoenix's atexit_common.head came up NULL, killing a process in
			 * _atexit_init() before main() with no diagnostic at all (2070 identical
			 * Data Aborts in one boot; libphoenix 160e916 has the userspace half).
			 *
			 * NFS READ can return 0 transiently, so retry a bounded number of times
			 * before accepting it. If it persists, still zero-fill -- failing the
			 * fault would turn a recoverable read into a dead process, and a
			 * genuinely truncated file must not wedge the pager -- but SAY SO, so the
			 * next occurrence is attributable instead of silent. */
			if (zeroRetry < 5u) {
				++zeroRetry;
				continue;
			}
			lib_printf("vm: object EOF at %u/%u bytes before its size (port %u, offs %u) -- "
					"zero-filling the remainder; a mapped page will read as zeros\n",
				(unsigned int)total, (unsigned int)span, (unsigned int)oid.port,
				(unsigned int)offs);
			break;
		}
		zeroRetry = 0u;
		if ((size_t)r > (span - total)) {
			/* A backing store must never report more bytes than it was asked
			 * for. Trusting it would push `total` past the window, and the
			 * per-page copy below decides how much to copy from `total` -- so
			 * every page would be copied in full out of a buffer whose tail was
			 * never written, handing the process uninitialised KERNEL HEAP. On
			 * this port that means the 0xba thread-stack fill plus whatever a
			 * recycled kernel stack left behind, written straight into a
			 * demand-paged .data page. Clamp, and say so, because a store that
			 * does this is itself broken. */
			lib_printf("vm: object read over-reported %u > %u bytes (port %u), clamping\n",
				(unsigned int)r, (unsigned int)(span - total), (unsigned int)oid.port);
			r = (int)(span - total);
		}
		total += (size_t)r;
	}

	(void)proc_close(oid, 0);

	if (err != EOK) {
		vm_kfree(buf);
		return err;
	}

	for (i = 0; i < want; ++i) {
		p = vm_pageAlloc(SIZE_PAGE, PAGE_OWNER_APP);
		if (p == NULL) {
			break;
		}

		v = vm_mmap(object_common.kmap, NULL, p, SIZE_PAGE, PROT_WRITE | PROT_USER, object_common.kernel, 0, MAP_NONE);
		if (v == NULL) {
			vm_pageFree(p);
			break;
		}

		/* Copy this page's slice of the bulk read; zero-fill the remainder (the EOF tail
		 * of the last page, or a page entirely beyond a short read). */
		target = (i * SIZE_PAGE < total) ? min(total - (i * SIZE_PAGE), (size_t)SIZE_PAGE) : 0u;
		if (target > 0u) {
			hal_memcpy(v, (char *)buf + (i * SIZE_PAGE), target);
		}
		if (target < SIZE_PAGE) {
			hal_memset((char *)v + target, 0, SIZE_PAGE - target);
		}

		(void)vm_munmap(object_common.kmap, v, SIZE_PAGE);
		out[i] = p;
		(*got)++;
	}

	vm_kfree(buf);

	/* The base page must exist for the faulting access to make progress. */
	if (*got == 0u) {
		return -ENOMEM;
	}

	return EOK;
}


int vm_objectPage(vm_map_t *map, amap_t **amap, vm_object_t *o, void *vaddr, u64 offs, page_t **page)
{
	int err;

	if (o == NULL) {
		*page = vm_pageAlloc(SIZE_PAGE, PAGE_OWNER_APP);
		return (*page != NULL) ? EOK : -ENOMEM;
	}

	if (o == VM_OBJ_PHYSMEM) {
		/* parasoft-suppress-next-line MISRAC2012-RULE_14_3 "Check is needed on targets where sizeof(offs) != sizeof(addr_t)" */
		if (offs > (addr_t)-1) {
			return -ERANGE;
		}
		*page = page_get((addr_t)offs);
		/* page can be NULL, when address outside of defined physical maps is used */
		return EOK;
	}

	(void)proc_lockSet(&object_common.lock);

	if (offs >= o->size) {
		(void)proc_lockClear(&object_common.lock);
		return -EINVAL;
	}

	*page = o->pages[offs / SIZE_PAGE];
	if (*page != NULL) {
		(void)proc_lockClear(&object_common.lock);
		return EOK;
	}

	/* Fetch page from backing store */

	(void)proc_lockClear(&object_common.lock);

	if (amap != NULL) {
		(void)proc_lockClear(&(*amap)->lock);
	}

	(void)proc_lockClear(&map->lock);

	{
		page_t *cluster[OBJECT_READAHEAD_PAGES];
		size_t got = 0, ci, baseIdx = (size_t)(offs / SIZE_PAGE);
		int fetchRc;

		fetchRc = object_fetchCluster(o->oid, offs, o->size, OBJECT_READAHEAD_PAGES, cluster, &got);
		if (fetchRc < 0) {
			got = 0;
		}

		*page = (got > 0u) ? cluster[0] : NULL;

		err = vm_lockVerify(map, amap, o, vaddr, offs);
		if (err != 0) {
			for (ci = 0; ci < got; ++ci) {
				vm_pageFree(cluster[ci]);
			}

			return err;
		}

		(void)proc_lockSet(&object_common.lock);

		/* Install the read-ahead pages (baseIdx+1 ..) into the object's page cache so the
		 * upcoming faults on them hit the cache instead of paying another round-trip. The
		 * base page (ci == 0) keeps the original "someone raced us in" handling below. The
		 * window was clamped to the object's backing pages, so baseIdx + ci is always in
		 * range. */
		for (ci = 1; ci < got; ++ci) {
			if (o->pages[baseIdx + ci] == NULL) {
				o->pages[baseIdx + ci] = cluster[ci];
			}
			else {
				vm_pageFree(cluster[ci]);
			}
		}

		if (o->pages[baseIdx] != NULL) {
			/* Someone loaded the base page in the meantime, use it */
			if (*page != NULL) {
				vm_pageFree(*page);
			}

			*page = o->pages[baseIdx];
		}
		else {
			o->pages[baseIdx] = *page;
		}

		(void)proc_lockClear(&object_common.lock);

		/* If the base page could not be fetched, surface the real backing-store
		 * error (e.g. -EIO from a failed NFS READ RPC) rather than letting the
		 * caller invent a generic -ENOMEM: a transient read failure must not
		 * masquerade as out-of-memory (that mislabelling turned an NFS read flake
		 * into a phantom "exec ENOMEM" that was diagnosed as a loader bug). */
		if (*page == NULL) {
			return (fetchRc < 0) ? fetchRc : -ENOMEM;
		}

		return EOK;
	}
}


vm_object_t *vm_objectContiguous(size_t size)
{
	vm_object_t *o;
	page_t *p;
	size_t i, n;

	p = vm_pageAlloc(size, PAGE_OWNER_APP);
	if (p == NULL) {
		return NULL;
	}

	size = 1UL << p->idx;
	n = size / SIZE_PAGE;

	o = vm_kmalloc(sizeof(vm_object_t) + n * sizeof(page_t *));
	if (o == NULL) {
		vm_pageFree(p);
		return NULL;
	}

	hal_memset(o, 0, sizeof(*o));
	/* Mark object as contiguous by setting its oid.port and oid.id to -1 */
	o->oid.port = (u32)(-1);
	o->oid.id = (id_t)(-1);
	o->refs = 1;
	o->size = size;

	for (i = 0; i < n; ++i) {
		o->pages[i] = p + i;
	}

	return o;
}


/*
 * Memory export
 *
 * A server exports memory it has mapped (MAP_CONTIGUOUS) by publishing an export window: an
 * object that borrows the pages of the source object and holds a reference to it. The window
 * is inserted into the object tree under the server's oid, so mmap() of any descriptor carrying
 * that oid finds it in vm_objectGet() and maps the same pages -- every page is present, so
 * object_fetchCluster() is never reached and the server is never asked for data.
 *
 * Lifetime: the export holds one reference to the window, every mapping holds another, and the
 * window holds one to its parent. The pages are freed only when the export is withdrawn
 * (vm_objectUnexport(), or release of the port) AND the last mapping is gone. A withdrawn
 * window leaves the tree at once, so its oid can never resolve to stale memory.
 *
 * Memory type: the window records the cache attributes of the source mapping, and every
 * mapping of it must request exactly those (vm_objectMapCheck()): a page is never mapped
 * cached in one place and uncached in another.
 */

int vm_objectExport(vm_map_t *map, oid_t oid, void *vaddr, size_t size)
{
	vm_object_t *o, *parent;
#if VM_OBJCACHE
	vm_object_t *stale;
#endif
	vm_flags_t flags;
	u64 offs;
	size_t i, n;
	int err;

	if ((size == 0U) || ((((ptr_t)vaddr | size) & (SIZE_PAGE - 1U)) != 0U)) {
		return -EINVAL;
	}

	/* The contiguous sentinel oid is not a name anyone can export under */
	if ((oid.port == (u32)(-1)) && (oid.id == (id_t)(-1))) {
		return -EINVAL;
	}

	n = size / SIZE_PAGE;
	o = vm_kmalloc(sizeof(vm_object_t) + n * sizeof(page_t *));
	if (o == NULL) {
		return -ENOMEM;
	}
	hal_memset(o, 0, sizeof(vm_object_t));

	err = vm_mapObjectRange(map, vaddr, size, &parent, &offs, &flags);
	if (err < 0) {
		vm_kfree(o);
		return err;
	}

	/* Only kernel-allocated anonymous contiguous memory: its pages are all present, owned by
	 * this object alone and never replaced. A file object's pages are a cache that can be
	 * refetched, and PHYSMEM has no page ownership at all. */
	if ((object_isContiguous(parent) == 0) || (offs > parent->size) || (size > (parent->size - offs))) {
		(void)vm_objectPut(parent);
		vm_kfree(o);
		return -EINVAL;
	}

	hal_memcpy(&o->oid, &oid, sizeof(oid));
	o->refs = 1; /* the export's reference */
	o->size = size;
	o->flags = (u8)(VM_OBJ_EXPORT | VM_OBJ_PUBLISHED);
	o->memtype = flags & (MAP_UNCACHED | MAP_DEVICE);
	o->parent = parent; /* takes over the reference from vm_mapObjectRange() */

	for (i = 0; i < n; ++i) {
		o->pages[i] = parent->pages[(size_t)(offs / SIZE_PAGE) + i];
	}

	(void)proc_lockSet(&object_common.lock);
#if VM_OBJCACHE
	/* An unreferenced file object under this oid would have been freed without the cache */
	stale = _object_find(oid);
	if ((stale != NULL) && ((stale->flags & VM_OBJ_CACHED) != 0U)) {
		_object_evict(stale);
	}
	else {
		stale = NULL;
	}
#endif
	/* -EEXIST also covers a file object that an early mmap() created under this oid */
	if (lib_rbInsert(&object_common.tree, &o->linkage) < 0) {
		(void)proc_lockClear(&object_common.lock);
#if VM_OBJCACHE
		object_freeList(stale);
#endif
		(void)vm_objectPut(parent);
		vm_kfree(o);
		return -EEXIST;
	}
	LIST_ADD(&object_common.exports, o);
	parent->flags |= (u8)VM_OBJ_SHARED;
	(void)proc_lockClear(&object_common.lock);

#if VM_OBJCACHE
	object_freeList(stale);
#endif

	return EOK;
}


int vm_objectUnexport(oid_t oid)
{
	vm_object_t t, *o;

	hal_memcpy(&t.oid, &oid, sizeof(oid));

	(void)proc_lockSet(&object_common.lock);
	o = lib_treeof(vm_object_t, linkage, lib_rbFind(&object_common.tree, &t.linkage));
	if ((o == NULL) || ((o->flags & VM_OBJ_PUBLISHED) == 0U)) {
		/* Not an export (e.g. a file object under the same oid): leave it alone */
		(void)proc_lockClear(&object_common.lock);
		return -ENOENT;
	}
	_object_unpublish(o);
	(void)proc_lockClear(&object_common.lock);

	return vm_objectPut(o);
}


void vm_objectUnexportPort(u32 port)
{
	vm_object_t *o;
	unsigned int n = 0;

	for (;;) {
		(void)proc_lockSet(&object_common.lock);
		o = object_common.exports;
		if (o != NULL) {
			while (o->oid.port != port) {
				o = o->next;
				if (o == object_common.exports) {
					o = NULL;
					break;
				}
			}
		}
		if (o != NULL) {
			_object_unpublish(o);
		}
		(void)proc_lockClear(&object_common.lock);

		if (o == NULL) {
			break;
		}
		(void)vm_objectPut(o);
		++n;
	}

	if (n != 0U) {
		/* Normal for an exporter that exits without withdrawing its exports; the pages
		 * stay with whoever still maps them. */
		lib_printf("vm: port %u released with %u memory export(s) still published, withdrawn\n", port, n);
	}

#if VM_OBJCACHE
	{
		vm_object_t *list = NULL;

		/* Forget the files of the server that owned the port: a new port with the same id
		 * must not find them */
		(void)proc_lockSet(&object_common.lock);
		for (;;) {
			o = object_common.lru;
			if (o != NULL) {
				while (o->oid.port != port) {
					o = o->next;
					if (o == object_common.lru) {
						o = NULL;
						break;
					}
				}
			}
			if (o == NULL) {
				break;
			}
			_object_evict(o);
			o->next = list;
			list = o;
		}
		(void)proc_lockClear(&object_common.lock);

		object_freeList(list);
	}
#endif
}


int vm_objectMapCheck(const vm_object_t *o, u64 offs, size_t size, vm_flags_t flags)
{
	if ((o == NULL) || (o == VM_OBJ_PHYSMEM) || ((o->flags & VM_OBJ_EXPORT) == 0U)) {
		return EOK;
	}

	if ((flags & (MAP_UNCACHED | MAP_DEVICE)) != o->memtype) {
		return -EINVAL;
	}

	if ((offs == VM_OFFS_MAX) || (offs > (u64)o->size) || ((u64)size > ((u64)o->size - offs))) {
		return -EINVAL;
	}

	return EOK;
}


int vm_objectShared(const vm_object_t *o)
{
	if ((o == NULL) || (o == VM_OBJ_PHYSMEM)) {
		return 0;
	}

	return ((o->flags & (VM_OBJ_EXPORT | VM_OBJ_SHARED)) != 0U) ? 1 : 0;
}


int _object_init(vm_map_t *kmap, vm_object_t *kernel)
{
	vm_object_t *o;

	lib_printf("vm: Initializing memory objects\n");

	object_common.kernel = kernel;
	object_common.kmap = kmap;

	(void)proc_lockInit(&object_common.lock, &proc_lockAttrDefault, "object.common");
	lib_rbInit(&object_common.tree, object_cmp, NULL);

	kernel->refs = 0;
	kernel->oid.port = 0;
	kernel->oid.id = 0;
	(void)lib_rbInsert(&object_common.tree, &kernel->linkage);

	(void)vm_objectGet(&o, kernel->oid);

#if VM_OBJCACHE
	{
		size_t freesz;

		vm_pageGetStats(&freesz);
		object_common.cacheMax = (freesz / SIZE_PAGE) / 100U * VM_OBJCACHE_PERCENT;
		object_common.lowWater = (freesz / SIZE_PAGE) / VM_OBJCACHE_LOWWATER;
		lib_printf("vm: Caching unreferenced file objects, up to %zu KB\n", object_common.cacheMax * (SIZE_PAGE / 1024U));
	}
#endif

	return EOK;
}
