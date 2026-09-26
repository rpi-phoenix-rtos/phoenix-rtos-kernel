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
#include "lib/lib.h"
#include "page.h"
#include "kmalloc.h"
#include "object.h"
#include "map.h"
#include "proc/name.h"
#include "proc/threads.h"


static struct {
	rbtree_t tree;
	vm_object_t *kernel;
	vm_map_t *kmap;
	vm_object_t *exports; /* published export windows, see vm_objectExport() */
	lock_t lock;
} object_common;


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


int vm_objectGet(vm_object_t **o, oid_t oid)
{
	vm_object_t t, *no = NULL;
	size_t i, n;
	off_t sz;
	int err = -ENOMEM;

	t.oid.port = oid.port;
	t.oid.id = oid.id;

	(void)proc_lockSet(&object_common.lock);
	*o = lib_treeof(vm_object_t, linkage, lib_rbFind(&object_common.tree, &t.linkage));

	if (*o == NULL) {
		/* Take off the lock to avoid a deadlock in vm_kmalloc */
		(void)proc_lockClear(&object_common.lock);

		sz = proc_size(oid);
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


		(void)proc_lockSet(&object_common.lock);
		/* Check again, somebody could've added the object in the meantime */
		*o = lib_treeof(vm_object_t, linkage, lib_rbFind(&object_common.tree, &t.linkage));
		if (*o == NULL) {
			if (no == NULL) {
				(void)proc_lockClear(&object_common.lock);
				return err;
			}
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

			for (i = 0; i < n; ++i) {
				(*o)->pages[i] = NULL;
			}

			(void)lib_rbInsert(&object_common.tree, &(*o)->linkage);
		}
	}

	(*o)->refs++;
	(void)proc_lockClear(&object_common.lock);

	/* Did we allocate an object we didn't need in the end? */
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
	unsigned int i;

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
		lib_rbRemove(&object_common.tree, &o->linkage);
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
		for (i = 0; i < round_page(o->size) / SIZE_PAGE; ++i) {
			if (o->pages[i] != NULL) {
				vm_pageFree(o->pages[i]);
			}
		}
	}

	vm_kfree(o);

	return EOK;
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
	/* -EEXIST also covers a file object that an early mmap() created under this oid */
	if (lib_rbInsert(&object_common.tree, &o->linkage) < 0) {
		(void)proc_lockClear(&object_common.lock);
		(void)vm_objectPut(parent);
		vm_kfree(o);
		return -EEXIST;
	}
	LIST_ADD(&object_common.exports, o);
	parent->flags |= (u8)VM_OBJ_SHARED;
	(void)proc_lockClear(&object_common.lock);

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

	return EOK;
}
