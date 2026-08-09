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
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
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
	lock_t lock;
} object_common;


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

	lib_rbRemove(&object_common.tree, &o->linkage);
	(void)proc_lockClear(&object_common.lock);

	/* Contiguous object 'holds' all pages in pages[0] */
	if ((o->oid.port == (u32)(-1)) && (o->oid.id == (id_t)(-1))) {
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
	while (total < span) {
		r = proc_read(oid, (off_t)(offs + total), (char *)buf + total, span - total, 0);
		if (r < 0) {
			err = r;
			break;
		}
		if (r == 0) {
			/* Server reported EOF earlier than the object size promised; the trailing
			 * zero-fill covers the gap rather than spinning. */
			break;
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
