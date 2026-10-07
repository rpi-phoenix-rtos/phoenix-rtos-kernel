/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Virtual memory manager - page allocator
 *
 * Copyright 2012, 2016 Phoenix Systems
 * Copyright 2001, 2005-2006 Pawel Pisarczyk
 * Author: Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "lib/lib.h"
#include "proc/proc.h"
#include "include/errno.h"
#include "include/mman.h"
#include "page.h"
#include "object.h"
#include "hal/types.h"


#define SIZE_VM_SIZES ((unsigned int)(sizeof(void *) * (size_t)__CHAR_BIT__))


/*
 * Grouping by mobility. Memory is divided into pageblocks (VM_PAGEBLOCK_PAGES pages, aligned),
 * and a pageblock that is split serves either the kernel or the processes, not both:
 *  - kernel pages (kmalloc zones -- e.g. an anon_t for every 32 pages of user memory --, page
 *    tables, amap arrays, kernel stacks) mostly outlive the user pages they were allocated among.
 *    Taken from the same blocks, they end up one every few pages all over memory, and nothing
 *    larger than the gaps between them can be had again however much is freed. (Simulation: a
 *    process takes a page table -- one page -- after every 2 MB another process touches, until
 *    that one has 3 GB; when it has exited, 3.3 GB are free and 50 blocks of 4 MB can be had
 *    without grouping, 781 with it.)
 *  - user pages and the file cache come and go with processes and files: a pageblock of them
 *    becomes free as a whole, for contiguous blocks (MAP_CONTIGUOUS, GPU buffers, big kmalloc
 *    zones).
 * Below the pageblock size each kind has its own free lists (sizes[PAGE_LIST_APP],
 * sizes[PAGE_LIST_KERNEL]); a free block of a pageblock or more is nobody's
 * (sizes[PAGE_LIST_BLOCKS]). An allocation takes the smallest block that fits from its own lists,
 * then from the shared ones -- splitting a free pageblock makes it its kind's (PAGE_BLOCK_KERNEL
 * on every page of it) --, and only then from the other kind's lists: an allocation fails only
 * if no free block of the lists would do. A free block lives on the list of its pageblock's
 * kind whoever freed it, so buddies (of a size below a pageblock, in one pageblock) are always on
 * the same list.
 */
#ifndef VM_PAGEBLOCK_PAGES
#define VM_PAGEBLOCK_PAGES 512U
#endif

#define PAGE_LIST_APP    0U
#define PAGE_LIST_KERNEL 1U
#define PAGE_LIST_BLOCKS 2U
#define PAGE_LISTS       3U

/* The page's pageblock serves the kernel: a page flag (bit 3) that no architecture uses */
#define PAGE_BLOCK_KERNEL 0x08U

_Static_assert(((PAGE_FREE | PAGE_OWNER_KERNEL | PAGE_OWNER_APP | PAGE_KERNEL_SYSPAGE | PAGE_KERNEL_CPU |
						PAGE_KERNEL_PTABLE | PAGE_KERNEL_PMAP | PAGE_KERNEL_STACK | PAGE_KERNEL_HEAP) &
					   PAGE_BLOCK_KERNEL) == 0U,
	"PAGE_BLOCK_KERNEL collides with a page flag of the architecture");
_Static_assert((VM_PAGEBLOCK_PAGES & (VM_PAGEBLOCK_PAGES - 1U)) == 0U, "VM_PAGEBLOCK_PAGES must be a power of 2");


static struct {
	page_t *sizes[PAGE_LISTS][SIZE_VM_SIZES];
	page_t *pages; /* pages ordering and their addresses (page_t::addr) stay invariant after _page_init() */

	size_t totalsz; /* stays invariant after _page_init() */
	size_t allocsz;
	size_t bootsz;
	unsigned int failures; /* allocations that failed, see page_reportFailure() */
	unsigned int blockidx; /* log2 of the pageblock size in bytes */

	lock_t lock;
} pages_info;


/* The free list of a free block of size 2^idx headed by p */
static page_t **_page_list(const page_t *p, unsigned int idx)
{
	unsigned int l;

	if (idx >= pages_info.blockidx) {
		l = PAGE_LIST_BLOCKS;
	}
	else {
		l = ((p->flags & PAGE_BLOCK_KERNEL) != 0U) ? PAGE_LIST_KERNEL : PAGE_LIST_APP;
	}

	return &pages_info.sizes[l][idx];
}


/* The smallest size from idx up to (not including) end with a block on list l, or end */
static unsigned int _page_fit(unsigned int l, unsigned int idx, unsigned int end)
{
	while ((idx < end) && (pages_info.sizes[l][idx] == NULL)) {
		idx++;
	}

	return idx;
}


static page_t *_page_alloc(size_t size, vm_flags_t flags)
{
	unsigned int start, stop, i, own, other, end = pages_info.blockidx;
	page_t *lh, *rh;

	/* Establish first index */
	size = (size < SIZE_PAGE) ? SIZE_PAGE : size;

	start = hal_cpuGetLastBit(size);
	/* parasoft-suppress-next-line MISRAC2012-RULE_14_3 "conditional compilation" */
	if (hal_cpuGetFirstBit(size) < start) {
		start++;
	}
	if (start >= SIZE_VM_SIZES) {
		return NULL;
	}

	if ((flags & PAGE_OWNER_APP) != 0U) {
		own = PAGE_LIST_APP;
		other = PAGE_LIST_KERNEL;
	}
	else {
		own = PAGE_LIST_KERNEL;
		other = PAGE_LIST_APP;
	}

	/* Smallest fit: in a pageblock of our kind, a free pageblock or more, the other kind */
	i = own;
	stop = _page_fit(own, start, end);
	if (stop >= end) {
		i = PAGE_LIST_BLOCKS;
		stop = _page_fit(PAGE_LIST_BLOCKS, max(start, end), SIZE_VM_SIZES);
		if (stop >= SIZE_VM_SIZES) {
			i = other;
			stop = _page_fit(other, start, end);
			if (stop >= end) {
				return NULL;
			}
		}
	}

	lh = pages_info.sizes[i][stop];
	LIST_REMOVE(&pages_info.sizes[i][stop], lh);

	/* Split segment, the right halves go back to the free lists */
	while (stop > start) {
		if (stop == pages_info.blockidx) {
			/* A whole free pageblock (lh is its first page): now it is ours */
			for (i = 0; i < VM_PAGEBLOCK_PAGES; i++) {
				if (own == PAGE_LIST_KERNEL) {
					(lh + i)->flags |= PAGE_BLOCK_KERNEL;
				}
				else {
					(lh + i)->flags &= (u8)~PAGE_BLOCK_KERNEL;
				}
			}
		}

		stop--;

		lh->idx = (u8)stop;
		rh = lh + ((size_t)1 << stop) / SIZE_PAGE;
		rh->idx = (u8)stop;
		LIST_ADD(_page_list(rh, stop), rh);
	}

	/* Mark allocated pages (the pageblock's kind stays) */
	for (i = 0; i < ((size_t)1 << lh->idx) / SIZE_PAGE; i++) {
		(lh + i)->flags = (u8)(((lh + i)->flags & PAGE_BLOCK_KERNEL) | flags);
		pages_info.allocsz += SIZE_PAGE;
	}

	return lh;
}


/*
 * Says why an allocation failed: memory used up, or free memory only in pieces smaller than the
 * request (a contiguous block, a kmalloc zone). Neither shows anywhere else -- the caller just
 * gets -ENOMEM -- and they call for different remedies. Bounded, so that a process faulting in a
 * loop cannot flood the console.
 */
static void page_reportFailure(size_t size)
{
	unsigned int idx, l, largest = 0U, n;
	size_t freesz;

	(void)proc_lockSet(&pages_info.lock);
	n = ++pages_info.failures;
	for (l = 0U; l < PAGE_LISTS; l++) {
		for (idx = 0U; idx < SIZE_VM_SIZES; idx++) {
			if ((pages_info.sizes[l][idx] != NULL) && (idx > largest)) {
				largest = idx;
			}
		}
	}
	freesz = pages_info.totalsz - pages_info.allocsz;
	(void)proc_lockClear(&pages_info.lock);

	/* The first ones, then a sample: a later failure of another kind still gets reported */
	if ((n <= 8U) || ((n % 64U) == 0U)) {
		lib_printf("vm: no free block of %zu KB (free %zu KB, largest free block %zu KB, file cache %zu KB; failure %u)\n",
			size / 1024U, freesz / 1024U, (freesz != 0U) ? (((size_t)1 << largest) / 1024U) : 0U,
			vm_objectCachedPages() * (SIZE_PAGE / 1024U), n);
	}
}


page_t *vm_pageAlloc(size_t size, vm_flags_t flags)
{
	page_t *p;

	/* Pages of unreferenced file objects are free memory to everybody else: give them up
	 * (vm_objectReclaim() frees one object's pages per call) until the allocation succeeds */
	for (;;) {
		(void)proc_lockSet(&pages_info.lock);
		p = _page_alloc(size, flags);
		(void)proc_lockClear(&pages_info.lock);

		if ((p != NULL) || (size > pages_info.totalsz) || (vm_objectReclaim() == 0)) {
			break;
		}
	}

	if (p == NULL) {
		page_reportFailure(size);
	}
	else {
		/* Keep memory that is really free, not only cached: pages given back at the last moment
		 * lie scattered between the ones taken meanwhile, and are no use for a contiguous block */
		(void)vm_objectReclaimLow(pages_info.totalsz - pages_info.allocsz);
	}

	return p;
}


void vm_pageFree(page_t *p)
{
	unsigned int idx, i;
	size_t pi, n, npages = pages_info.totalsz / SIZE_PAGE;
	page_t *lh, *rh, *buddy;

	if (p == NULL) {
		return;
	}

	(void)proc_lockSet(&pages_info.lock);

	if ((p->flags & PAGE_FREE) != 0U) {
		hal_cpuDisableInterrupts();
		lib_printf("page: double free (%p)\n", p);
		hal_cpuEnableInterrupts();
		for (;;) {
		}
	}

	idx = p->idx;

	/* Mark free pages */
	for (i = 0; i < ((u64)1 << idx) / SIZE_PAGE; i++) {
		(p + i)->flags |= PAGE_FREE;
		pages_info.allocsz -= SIZE_PAGE;
	}

	/* Merge with the buddy for as long as it is a free block of the same size. Both halves are
	 * worked out afresh at every size: a block that grew as the left half may be the right half
	 * of the next size. (Before, only the pointer to the buddy was moved, so merging stopped at
	 * the first such size, and memory never became one block again. On a 4 GB Pi 4, after a
	 * process used up memory and exited: 2.9 GB free, the largest block 32 KB.) */
	while ((idx + 1U) < SIZE_VM_SIZES) {
		/* parasoft-suppress-next-line MISRAC2012-RULE_18_4 "p points into pages_info.pages" */
		pi = (size_t)(p - pages_info.pages);
		n = (size_t)(((u64)1 << idx) / SIZE_PAGE);

		if ((p->addr & ((u64)1 << idx)) != 0U) {
			if (pi < n) {
				break;
			}
			lh = p - n;
			rh = p;
			buddy = lh;
		}
		else {
			if ((pi + n) >= npages) {
				break;
			}
			lh = p;
			rh = p + n;
			buddy = rh;
		}

		/* The page at the buddy's place heads a free block of this size, and the two are
		 * contiguous (there may be a hole in physical memory between neighbouring page_t) */
		if (((buddy->flags & PAGE_FREE) == 0U) || (buddy->idx != idx) || ((lh->addr + ((u64)1 << idx)) != rh->addr)) {
			break;
		}

		LIST_REMOVE(_page_list(buddy, idx), buddy);

		rh->idx = (u8)hal_cpuGetFirstBit(SIZE_PAGE);
		idx++;
		lh->idx = (u8)idx;
		p = lh;
	}

	LIST_ADD(_page_list(p, idx), p);

	(void)proc_lockClear(&pages_info.lock);
	return;
}


static int page_get_cmp(void *key, void *item)
{
	addr_t a = (addr_t)key;
	page_t *p = (page_t *)item;

	if (a == p->addr) {
		return 0;
	}

	if (a > p->addr) {
		return 1;
	}

	return -1;
}


page_t *page_get(addr_t addr)
{
	page_t *p;
	size_t np = pages_info.totalsz / SIZE_PAGE;

	addr = addr & ~(SIZE_PAGE - 1U);
	p = lib_bsearch((void *)addr, pages_info.pages, np, sizeof(page_t), page_get_cmp);

	return p;
}


static void _page_initSizes(void)
{
	unsigned int idx;
	size_t k, i = 0;
	page_t *p;

	/* Remove already discovered pages */
	pages_info.sizes[PAGE_LIST_APP][hal_cpuGetFirstBit(SIZE_PAGE)] = NULL;

	while (i < pages_info.totalsz / SIZE_PAGE) {
		p = &pages_info.pages[i];
		if ((p->flags & PAGE_FREE) == 0U) {
			i++;
			continue;
		}

		idx = hal_cpuGetFirstBit(p->addr);

		if (idx >= SIZE_VM_SIZES) {
			idx = SIZE_VM_SIZES - 1U;
		}

		/* Free pages that follow without a hole in physical memory (the page_t of the first page
		 * after a hole is the next one in pages_info.pages) */
		/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "idx is limited to min(SIZE_VM_SIZES - 1U, bits in p-> addr")*/
		for (k = 0U; (k < (((u64)1 << idx) / SIZE_PAGE) - 1U) && (i + k < ((pages_info.totalsz / SIZE_PAGE) - 1U)); k++) {
			if (((pages_info.pages[i + k + 1U].flags & PAGE_FREE) == 0U) ||
					(pages_info.pages[i + k + 1U].addr != (p->addr + (k + 1U) * SIZE_PAGE))) {
				break;
			}
		}

		idx = hal_cpuGetLastBit((k + 1U) * SIZE_PAGE);
		p->idx = (u8)idx;

		LIST_ADD(_page_list(p, idx), p);

		i += (size_t)(((u64)1 << idx) / SIZE_PAGE);
	}
	return;
}


static unsigned int page_digits(unsigned int n, unsigned int base)
{
	unsigned int d = 0;

	do {
		n /= base;
		d++;
	} while (n != 0U);

	return d;
}


#define TTY_COLS 80U
void _page_showPages(void)
{
	addr_t a = 0;
	page_t *p;
	unsigned int rep, i = 0, k;
	int w;
	char c;
	char buf[TTY_COLS + 1U];

	w = lib_sprintf(buf, "vm: ");
	while (i < pages_info.totalsz / SIZE_PAGE) {
		p = &pages_info.pages[i];

		/* Print markers in case of memory gap */
		if (p->addr > a) {
			rep = (unsigned int)(p->addr - a) / (unsigned int)SIZE_PAGE;
			if (rep >= 4U) {
				k = page_digits(rep, 10) + 3U;
				if ((unsigned int)w + k > TTY_COLS) {
					lib_printf("%s\n", buf);
					w = lib_sprintf(buf, "vm: ");
				}
				w += lib_sprintf(buf + w, "[%dx]", rep);
			}
			else {
				for (k = 0; k < rep; k++) {
					if ((unsigned int)w + 1U > TTY_COLS) {
						lib_printf("%s\n", buf);
						w = lib_sprintf(buf, "vm: ");
					}
					w += lib_sprintf(buf + w, "%c", 'x');
				}
			}
		}

		/* Print markers with repetitions */
		c = pmap_marker(p);
		for (rep = 0; ((size_t)i + rep + 1U) < pages_info.totalsz / SIZE_PAGE; rep++) {
			if ((c != pmap_marker(&pages_info.pages[i + rep + 1U])) || (pages_info.pages[i + rep + 1U].addr - pages_info.pages[i + rep].addr > SIZE_PAGE)) {
				break;
			}
		}

		if (rep >= 4U) {
			k = page_digits(rep + 1U, 10) + 3U;
			if ((unsigned int)w + k > TTY_COLS) {
				lib_printf("%s\n", buf);
				w = lib_sprintf(buf, "vm: ");
			}
			w += lib_sprintf(buf + w, "[%d%c]", rep + 1U, c);
		}
		else {
			for (k = 0; k <= rep; k++) {
				if ((unsigned int)w + 1U > TTY_COLS) {
					lib_printf("%s\n", buf);
					w = lib_sprintf(buf, "vm: ");
				}
				w += lib_sprintf(buf + w, "%c", pmap_marker(p));
			}
		}

		a = pages_info.pages[i + rep].addr + SIZE_PAGE;
		i += rep + 1U;
	}

	if (w > 4) {
		lib_printf("%s\n", buf);
	}

	return;
}


static int _page_map(pmap_t *pmap, void *vaddr, addr_t pa, vm_attr_t attr)
{
	page_t *ap = NULL;

	while (pmap_enter(pmap, pa, vaddr, attr, ap) < 0) {
		ap = _page_alloc(SIZE_PAGE, PAGE_OWNER_KERNEL | PAGE_KERNEL_PTABLE);
		if (/*vaddr > (void *)VADDR_KERNEL ||*/ ap == NULL) {
			return -ENOMEM;
		}
	}
	return EOK;
}


int page_map(pmap_t *pmap, void *vaddr, addr_t pa, vm_attr_t attr)
{
	int err;

	/* A page table is allocated under pages_info.lock: reclaim cached file pages with it
	 * dropped (see vm_pageAlloc()). A retry continues where pmap_enter() stopped. */
	for (;;) {
		(void)proc_lockSet(&pages_info.lock);
		err = _page_map(pmap, vaddr, pa, attr);
		(void)proc_lockClear(&pages_info.lock);

		if ((err != -ENOMEM) || (vm_objectReclaim() == 0)) {
			break;
		}
	}

	return err;
}


int _page_sbrk(pmap_t *pmap, void **start, void **end)
{
	page_t *np, *ap = NULL;
	np = _page_alloc(SIZE_PAGE, PAGE_OWNER_KERNEL | PAGE_KERNEL_HEAP);
	if (np == NULL) {
		return -ENOMEM;
	}

	while (pmap_enter(pmap, np->addr, (*end), PGHD_READ | PGHD_WRITE | PGHD_PRESENT, ap) < 0) {
		ap = _page_alloc(SIZE_PAGE, PAGE_OWNER_KERNEL | PAGE_KERNEL_PTABLE);
		if (ap == NULL) {
			return -ENOMEM;
		}
	}

	(*end) += SIZE_PAGE;

	return EOK;
}


void vm_pageGetStats(size_t *freesz)
{
	*freesz = pages_info.totalsz - pages_info.allocsz;
}


void vm_pageinfo(meminfo_t *info)
{
	char c;
	page_t *p;
	unsigned int rep, i = 0;
	int size = 0;
	int mapsz = info->page.mapsz;
	pageinfo_t *map = info->page.map;
	size_t cached = vm_objectCachedPages() * SIZE_PAGE;

	if (mapsz != -1) {
		/* mapsz * sizeof(map[0]) would overflow */
		if (mapsz < -1 || (size_t)mapsz > (size_t)-1 / sizeof(map[0])) {
			return;
		}

		if (vm_mapBelongs(proc_current()->process, map, (size_t)mapsz * sizeof(map[0])) < 0) {
			return;
		}
	}

	(void)proc_lockSet(&pages_info.lock);

	/* Cached pages of unreferenced file objects are given up on demand: report them as free */
	cached = min(cached, pages_info.allocsz);
	info->page.alloc = (unsigned int)(pages_info.allocsz - cached);
	info->page.free = (unsigned int)(pages_info.totalsz - pages_info.allocsz + cached);
	info->page.boot = (unsigned int)pages_info.bootsz;
	info->page.sz = (unsigned int)sizeof(page_t);

	if (mapsz != -1) {
		while (i < pages_info.totalsz / SIZE_PAGE) {
			p = pages_info.pages + i;

			c = pmap_marker(p);
			for (rep = 0; ((size_t)i + rep + 1U) < pages_info.totalsz / SIZE_PAGE; rep++) {
				if ((c != pmap_marker(pages_info.pages + i + rep + 1U)) || ((pages_info.pages[i + rep + 1U].addr - pages_info.pages[i + rep].addr) > SIZE_PAGE)) {
					break;
				}
			}

			if (mapsz > size && map != NULL) {
				map[size].count = rep + 1U;
				map[size].marker = c;
				map[size].addr = p->addr;
			}

			i += rep + 1U;
			++size;
		}

		info->page.mapsz = size;
	}

	(void)proc_lockClear(&pages_info.lock);
}


void _page_init(pmap_t *pmap, void **bss, void **top)
{
	addr_t addr;
	unsigned int k;
	page_t *page, *p;
	int err;
	void *vaddr;

	(void)proc_lockInit(&pages_info.lock, &proc_lockAttrDefault, "page");

	/* Prepare memory hash */
	pages_info.totalsz = 0;
	pages_info.allocsz = 0;
	pages_info.bootsz = 0;
	pages_info.failures = 0;

	for (k = 0; k < SIZE_VM_SIZES; k++) {
		pages_info.sizes[PAGE_LIST_APP][k] = NULL;
		pages_info.sizes[PAGE_LIST_KERNEL][k] = NULL;
		pages_info.sizes[PAGE_LIST_BLOCKS][k] = NULL;
	}
	pages_info.blockidx = hal_cpuGetFirstBit(VM_PAGEBLOCK_PAGES * SIZE_PAGE);

	addr = 0;
	pages_info.pages = (page_t *)*bss;
	page = (page_t *)*bss;

	for (;;) {
		if ((void *)page + sizeof(page_t) >= (*top)) {
			if (_page_sbrk(pmap, bss, top) < 0) {
				lib_printf("vm: Kernel heap extension error %p %p!\n", page, *top);
				return;
			}
		}

		err = pmap_getPage(page, &addr);
		if (err == -ENOMEM) {
			break;
		}

		if (err == EOK) {
			/* Every pageblock starts out as user memory, whatever the HAL left in the flags */
			page->flags &= (u8)~PAGE_BLOCK_KERNEL;
			pages_info.totalsz += SIZE_PAGE;
			if ((page->flags & PAGE_FREE) != 0U) {
				page->idx = (u8)hal_cpuGetFirstBit(SIZE_PAGE);
				LIST_ADD(&pages_info.sizes[PAGE_LIST_APP][hal_cpuGetFirstBit(SIZE_PAGE)], page);
			}
			else {
				page->idx = 0;
				pages_info.allocsz += SIZE_PAGE;
				if (((page->flags >> 1U) & 7U) == PAGE_OWNER_BOOT) {
					pages_info.bootsz += SIZE_PAGE;
				}
			}
			page = page + 1;
		}

		/* Wrap over 0 */
		if (addr < SIZE_PAGE) {
			break;
		}
	}

	(*bss) = page;

	/* Prepare allocation hash */
	_page_initSizes();

	/* Initialize kernel space for user processes */
	p = NULL;
	vaddr = (*top);

	for (;;) {
		if (_pmap_kernelSpaceExpand(pmap, &vaddr, (*top) + max(pages_info.totalsz / 4U, (1UL << 23)), p) == 0) {
			break;
		}
		p = _page_alloc(SIZE_PAGE, PAGE_OWNER_KERNEL | PAGE_KERNEL_PTABLE);
		if (p == NULL) {
			return;
		}
	}

	/* Show statistics on the console */
	lib_printf("vm: Initializing page allocator (%d+%d)/%dKB, page_t=%d, pageblocks of %uKB\n", (pages_info.allocsz - pages_info.bootsz) / 1024U,
			pages_info.bootsz / 1024U, pages_info.totalsz / 1024U, sizeof(page_t), (unsigned int)((VM_PAGEBLOCK_PAGES * SIZE_PAGE) / 1024U));

	_page_showPages();

	/* Create NULL pointer entry */
	(void)_page_map(pmap, NULL, 0, PGHD_USER | ~PGHD_PRESENT);

	return;
}
