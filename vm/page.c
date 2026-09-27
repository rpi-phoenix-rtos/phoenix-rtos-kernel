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
#include "hal/types.h"


#define SIZE_VM_SIZES ((unsigned int)(sizeof(void *) * (size_t)__CHAR_BIT__))


static struct {
	page_t *sizes[SIZE_VM_SIZES];
	page_t *pages; /* pages ordering and their addresses (page_t::addr) stay invariant after _page_init() */

	size_t totalsz; /* stays invariant after _page_init() */
	size_t allocsz;
	size_t bootsz;

	lock_t lock;
} pages_info;


#ifdef C1_PAGE_PROVENANCE
static void _page_provAlloc(const page_t *lh, vm_flags_t flags);
static void _page_provFree(const page_t *p);
#endif


static page_t *_page_alloc(size_t size, vm_flags_t flags)
{
	unsigned int start, stop, i;
	page_t *lh, *rh;

	/* Establish first index */
	size = (size < SIZE_PAGE) ? SIZE_PAGE : size;

	start = hal_cpuGetLastBit(size);
	/* parasoft-suppress-next-line MISRAC2012-RULE_14_3 "conditional compilation" */
	if (hal_cpuGetFirstBit(size) < start) {
		start++;
	}

	/* Find segment */
	stop = start;

	while ((stop < SIZE_VM_SIZES) && (pages_info.sizes[stop] == NULL)) {
		stop++;
	}
	if (stop == SIZE_VM_SIZES) {
		return NULL;
	}

	lh = pages_info.sizes[stop];

	/* Split segment */
	while (stop > start) {
		LIST_REMOVE(&pages_info.sizes[stop], lh);

		stop--;

		lh->idx--;
		rh = lh + (1UL << lh->idx) / SIZE_PAGE;
		rh->idx = lh->idx;
		LIST_ADD(&pages_info.sizes[stop], lh);
		LIST_ADD(&pages_info.sizes[stop], rh);
	}

	LIST_REMOVE(&pages_info.sizes[stop], lh);

	/* Mark allocated pages */
	for (i = 0; i < (1UL << lh->idx) / SIZE_PAGE; i++) {
		(lh + i)->flags &= ~PAGE_FREE;
		(lh + i)->flags |= flags;
		pages_info.allocsz += SIZE_PAGE;
	}

#ifdef C1_PAGE_PROVENANCE
	_page_provAlloc(lh, flags);
#endif

	return lh;
}


page_t *vm_pageAlloc(size_t size, vm_flags_t flags)
{
	page_t *p;

	(void)proc_lockSet(&pages_info.lock);
	p = _page_alloc(size, flags);
	(void)proc_lockClear(&pages_info.lock);
	return p;
}


void vm_pageFree(page_t *p)
{
	unsigned int idx, i;
	page_t *lh = p, *rh = p;

	if (p == NULL) {
		return;
	}

	(void)proc_lockSet(&pages_info.lock);

	if ((lh->flags & PAGE_FREE) != 0U) {
		hal_cpuDisableInterrupts();
		lib_printf("page: double free (%p)\n", lh);
		hal_cpuEnableInterrupts();
		for (;;) {
		}
	}

	idx = p->idx;

#ifdef C1_PAGE_PROVENANCE
	_page_provFree(p);
#endif

	/* Mark free pages */
	for (i = 0; i < ((u64)1 << idx) / SIZE_PAGE; i++) {
		(p + i)->flags |= PAGE_FREE;
		pages_info.allocsz -= SIZE_PAGE;
	}

	if ((p->addr & (((u64)1 << (idx + 1U)) - 1U)) != 0U) {
		lh = p - ((u64)1 << idx) / SIZE_PAGE;
	}
	else {
		rh = p + ((u64)1 << idx) / SIZE_PAGE;
	}

	/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 MISRAC2012-RULE_18_3 "lh, rh, pages_info.pages are related" */
	while ((lh >= pages_info.pages) && (rh < (pages_info.pages + pages_info.totalsz / SIZE_PAGE)) &&
			((lh->flags & PAGE_FREE) != 0U) && ((rh->flags & PAGE_FREE) != 0U) && (lh->idx == rh->idx) &&
			((lh->addr + (1UL << lh->idx)) == rh->addr) && (idx < SIZE_VM_SIZES)) {

		if (p == lh) {
			LIST_REMOVE(&pages_info.sizes[idx], rh);
		}
		else {
			LIST_REMOVE(&pages_info.sizes[idx], lh);
		}

		rh->idx = (u8)hal_cpuGetFirstBit(SIZE_PAGE);
		lh->idx++;
		idx++;

		p = lh;

		if ((p->addr & (((u64)1 << (idx + 1U)) - 1U)) != 0U) {
			lh = p - ((u64)1 << idx) / SIZE_PAGE;
		}
		else {
			rh = p + ((u64)1 << idx) / SIZE_PAGE;
		}
	}

	LIST_ADD(&pages_info.sizes[idx], p);

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
	pages_info.sizes[hal_cpuGetFirstBit(SIZE_PAGE)] = NULL;

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

		/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "idx is limited to min(SIZE_VM_SIZES - 1U, bits in p-> addr")*/
		for (k = 0U; (k < (((u64)1 << idx) / SIZE_PAGE) - 1U) && (i + k < ((pages_info.totalsz / SIZE_PAGE) - 1U)); k++) {
			if ((pages_info.pages[i + k + 1U].flags & PAGE_FREE) == 0U) {
				break;
			}
		}

		idx = hal_cpuGetLastBit((k + 1U) * SIZE_PAGE);
		p->idx = (u8)idx;

		LIST_ADD(&pages_info.sizes[idx], p);

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

	(void)proc_lockSet(&pages_info.lock);
	err = _page_map(pmap, vaddr, pa, attr);
	(void)proc_lockClear(&pages_info.lock);

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

	info->page.alloc = (unsigned int)pages_info.allocsz;
	info->page.free = (unsigned int)(pages_info.totalsz - pages_info.allocsz);
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

	for (k = 0; k < SIZE_VM_SIZES; k++) {
		pages_info.sizes[k] = NULL;
	}

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
			pages_info.totalsz += SIZE_PAGE;
			if ((page->flags & PAGE_FREE) != 0U) {
				page->idx = (u8)hal_cpuGetFirstBit(SIZE_PAGE);
				LIST_ADD(&pages_info.sizes[hal_cpuGetFirstBit(SIZE_PAGE)], page);
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
	lib_printf("vm: Initializing page allocator (%d+%d)/%dKB, page_t=%d\n", (pages_info.allocsz - pages_info.bootsz) / 1024U,
			pages_info.bootsz / 1024U, pages_info.totalsz / 1024U, sizeof(page_t));

	_page_showPages();

	/* Create NULL pointer entry */
	(void)_page_map(pmap, NULL, 0, PGHD_USER | ~PGHD_PRESENT);

	return;
}


#ifdef C1_PAGE_PROVENANCE

/*
 * TODO(C1-hunt): physical-page provenance log (-DC1_PAGE_PROVENANCE, default off).
 *
 * C1 writes 0x8000000x at page+4 of pages that sit in one fixed physical band. The userspace
 * detector sees the victim long after the write and knows only its physical address, so it
 * cannot say who held that page before malloc did. This log can: every allocation, free and
 * user MAP_PHYSMEM mapping touching a page of the band is recorded in that page's own ring,
 * and meminfo() with C1PROV_MEMINFO_MAGIC prints the ring (see syscalls_meminfo()).
 *
 * One ring per page rather than one global ring, so that the band's churn cannot evict the
 * history of the one page that matters. Everything is static: nothing here allocates, and
 * recording runs under pages_info.lock, which every caller of _page_alloc() and
 * vm_pageFree() already holds (_page_sbrk() only runs single-threaded, from _page_init()).
 * Printing never happens under that lock: vm_pageProvDump() copies a ring out first.
 */

#ifndef C1PROV_BAND_LO
#define C1PROV_BAND_LO 0x08000000U
#endif
#ifndef C1PROV_BAND_HI
#define C1PROV_BAND_HI 0x08600000U
#endif

#define C1PROV_PAGES      ((C1PROV_BAND_HI - C1PROV_BAND_LO) / SIZE_PAGE)
#define C1PROV_DEPTH      8U
#define C1PROV_DUMP_PAGES 4U
#define C1PROV_LINE       256U

#define C1PROV_EV_ALLOC 1U
#define C1PROV_EV_FREE  2U
#define C1PROV_EV_PHYS  3U

/* Kinds past the PAGE_PROV_* ones a call site can name (vm/page.h) */
#define C1PROV_KIND_APP     7U /* PAGE_OWNER_APP, call site did not say */
#define C1PROV_KIND_PTABLE  8U
#define C1PROV_KIND_PMAP    9U
#define C1PROV_KIND_KSTACK  10U
#define C1PROV_KIND_KHEAP   11U
#define C1PROV_KIND_KERNEL  12U /* any other PAGE_OWNER_KERNEL */
#define C1PROV_KIND_BOOT    13U
#define C1PROV_KIND_PHYS    14U /* MAP_PHYSMEM, cached */
#define C1PROV_KIND_PHYSUC  15U /* MAP_PHYSMEM | MAP_UNCACHED */
#define C1PROV_KIND_PHYSDEV 16U /* MAP_PHYSMEM | MAP_DEVICE */
#define C1PROV_KINDS        17U


typedef struct {
	u32 ms;     /* hal_timerGetUs() / 1000 */
	s32 pid;    /* -1: before the scheduler started, 0: kernel thread */
	u32 head;   /* first page (PFN) of the block or mapping */
	u16 npages; /* size of the block or mapping in pages, saturated */
	u8 ev;
	u8 kind;
} page_provEv_t;


static struct {
	page_provEv_t ev[C1PROV_PAGES][C1PROV_DEPTH];
	u32 total[C1PROV_PAGES]; /* events ever recorded; ring slot = total % C1PROV_DEPTH */
	u8 kind[C1PROV_PAGES];   /* kind of the page's latest allocation, carried to its free */
	unsigned int nextKind;   /* vm_pageAllocProv() -> _page_alloc(), under pages_info.lock */
	unsigned int dumps;
} page_prov;


static const char *const page_provKindName[C1PROV_KINDS] = {
	"none", "anon", "kanon", "cow", "file", "contig", "msg", "app", "ptable", "pmap",
	"kstack", "kheap", "kernel", "boot", "phys", "physuc", "physdev"
};


static const char *const page_provEvName[4] = { "none", "alloc", "free", "phys" };


static s32 page_provPid(void)
{
	thread_t *t;

	/* proc_current() spins on the threads spinlock, which is not initialised (held) until
	 * _proc_init() -- and the first allocations happen in _page_init(), long before that */
	if (hal_started() == 0) {
		return -1;
	}

	t = proc_current();

	return ((t != NULL) && (t->process != NULL)) ? (s32)process_getPid(t->process) : 0;
}


static unsigned int page_provKindOf(vm_flags_t flags)
{
	if ((flags & (7U << 1)) == PAGE_OWNER_APP) {
		return C1PROV_KIND_APP;
	}

	if ((flags & (7U << 1)) == PAGE_OWNER_KERNEL) {
		switch (flags & (7U << 4)) {
			case PAGE_KERNEL_PTABLE:
				return C1PROV_KIND_PTABLE;
			case PAGE_KERNEL_PMAP:
				return C1PROV_KIND_PMAP;
			case PAGE_KERNEL_STACK:
				return C1PROV_KIND_KSTACK;
			case PAGE_KERNEL_HEAP:
				return C1PROV_KIND_KHEAP;
			default:
				return C1PROV_KIND_KERNEL;
		}
	}

	return C1PROV_KIND_BOOT;
}


/* Records ev for every band page of [head, head + npages). Called with pages_info.lock held. */
static void _page_provNote(addr_t head, size_t npages, unsigned int ev, unsigned int kind)
{
	addr_t a, end;
	page_provEv_t *e;
	size_t i;
	u32 ms;
	s32 pid;

	if ((head >= C1PROV_BAND_HI) || (npages == 0U)) {
		return;
	}

	end = (npages >= (C1PROV_BAND_HI - head) / SIZE_PAGE) ? C1PROV_BAND_HI : (head + npages * SIZE_PAGE);
	if (end <= C1PROV_BAND_LO) {
		return;
	}

	ms = (u32)(hal_timerGetUs() / 1000);
	pid = page_provPid();

	for (a = (head < C1PROV_BAND_LO) ? C1PROV_BAND_LO : head; a < end; a += SIZE_PAGE) {
		i = (size_t)((a - C1PROV_BAND_LO) / SIZE_PAGE);

		if (ev == C1PROV_EV_ALLOC) {
			page_prov.kind[i] = (u8)kind;
		}

		e = &page_prov.ev[i][page_prov.total[i] % C1PROV_DEPTH];
		e->ms = ms;
		e->pid = pid;
		e->head = (u32)(head / SIZE_PAGE);
		e->npages = (npages > 0xffffU) ? 0xffffU : (u16)npages;
		e->ev = (u8)ev;
		/* A free is reported as the kind of the allocation it ends */
		e->kind = (ev == C1PROV_EV_FREE) ? page_prov.kind[i] : (u8)kind;
		page_prov.total[i]++;
	}
}


static void _page_provAlloc(const page_t *lh, vm_flags_t flags)
{
	unsigned int kind = page_prov.nextKind;

	if (kind == PAGE_PROV_NONE) {
		kind = page_provKindOf(flags);
	}

	_page_provNote(lh->addr, ((size_t)1 << lh->idx) / SIZE_PAGE, C1PROV_EV_ALLOC, kind);
}


static void _page_provFree(const page_t *p)
{
	_page_provNote(p->addr, ((size_t)1 << p->idx) / SIZE_PAGE, C1PROV_EV_FREE, PAGE_PROV_NONE);
}


page_t *vm_pageAllocProv(size_t size, vm_flags_t flags, unsigned int kind)
{
	page_t *p;

	(void)proc_lockSet(&pages_info.lock);
	page_prov.nextKind = kind;
	p = _page_alloc(size, flags);
	page_prov.nextKind = PAGE_PROV_NONE;
	(void)proc_lockClear(&pages_info.lock);

	return p;
}


void vm_pageProvPhys(addr_t pa, size_t size, vm_flags_t flags)
{
	unsigned int kind = C1PROV_KIND_PHYS;

	if ((flags & MAP_DEVICE) != 0U) {
		kind = C1PROV_KIND_PHYSDEV;
	}
	else if ((flags & MAP_UNCACHED) != 0U) {
		kind = C1PROV_KIND_PHYSUC;
	}
	else {
		/* No action required */
	}

	(void)proc_lockSet(&pages_info.lock);
	_page_provNote(pa & ~((addr_t)SIZE_PAGE - 1U), (size + SIZE_PAGE - 1U) / SIZE_PAGE, C1PROV_EV_PHYS, kind);
	(void)proc_lockClear(&pages_info.lock);
}


/*
 * Prints one C1PROV line. The UART corrupts about one line in a hundred and a flipped hex
 * digit still parses, so every line ends in ck=: the byte sum, mod 0x10000, of the line text
 * from "C1PROV" up to (not including) " ck=". A reader drops any line whose ck disagrees.
 */
static void page_provLine(char *buf, int len)
{
	unsigned int ck = 0;
	int k;

	for (k = 0; k < len; k++) {
		ck += (unsigned char)buf[k];
	}

	lib_printf("%s ck=%04x\n", buf, ck & 0xffffU);
}


/* Appends " who=<path>" of the process that holds pid now (pids are not reused before MAX_PID) */
static int page_provWho(char *buf, s32 pid)
{
	process_t *proc;
	const char *s = "-";
	int n = 0;

	proc = (pid > 0) ? proc_find(pid) : NULL;
	if ((proc != NULL) && (proc->path != NULL)) {
		s = proc->path;
	}
	else if (pid == 0) {
		s = "kernel";
	}
	else if (pid < 0) {
		s = "boot";
	}
	else {
		/* No action required */
	}

	n = lib_sprintf(buf, " who=");
	for (; (*s != '\0') && (n < 5 + 32); s++) {
		buf[n++] = ((*s == ' ') || (*s == '\t')) ? '_' : *s;
	}
	buf[n] = '\0';

	if (proc != NULL) {
		(void)proc_put(proc);
	}

	return n;
}


unsigned int vm_pageProvDump(addr_t pa, size_t npages, unsigned int reason)
{
	page_provEv_t ev[C1PROV_DEPTH];
	const page_provEv_t *e;
	char buf[C1PROV_LINE];
	unsigned int dump, lines = 0, kind, pflags;
	u32 total, j, n, now;
	size_t k, i;
	page_t *pg;
	s32 caller = page_provPid();
	int len;

	if (npages == 0U) {
		npages = 1;
	}
	if (npages > C1PROV_DUMP_PAGES) {
		npages = C1PROV_DUMP_PAGES;
	}

	pa &= ~((addr_t)SIZE_PAGE - 1U);

	(void)proc_lockSet(&pages_info.lock);
	dump = ++page_prov.dumps;
	(void)proc_lockClear(&pages_info.lock);

	for (k = 0; k < npages; k++, pa += SIZE_PAGE) {
		now = (u32)(hal_timerGetUs() / 1000);

		if ((pa < C1PROV_BAND_LO) || (pa >= C1PROV_BAND_HI)) {
			len = lib_sprintf(buf, "C1PROV d=%u.%u pa=0x%llx ev=oob pid=%d tick=%u npages=0 kind=none band=0x%x-0x%x",
				dump, reason, (unsigned long long)pa, caller, now, C1PROV_BAND_LO, C1PROV_BAND_HI);
			page_provLine(buf, len);
			lines++;
			continue;
		}

		i = (size_t)((pa - C1PROV_BAND_LO) / SIZE_PAGE);
		pg = page_get(pa);

		(void)proc_lockSet(&pages_info.lock);
		total = page_prov.total[i];
		kind = page_prov.kind[i];
		pflags = (pg != NULL) ? pg->flags : 0x100U;
		hal_memcpy(ev, page_prov.ev[i], sizeof(ev));
		(void)proc_lockClear(&pages_info.lock);

		/* Header: the page as it is now, and how much of its history the ring still holds */
		len = lib_sprintf(buf, "C1PROV d=%u.%u pa=0x%llx ev=now pid=%d tick=%u npages=1 kind=%s pflags=0x%x free=%u total=%u depth=%u",
			dump, reason, (unsigned long long)pa, caller, now, page_provKindName[(kind < C1PROV_KINDS) ? kind : 0U],
			pflags, ((pflags & PAGE_FREE) != 0U) ? 1U : 0U, total, C1PROV_DEPTH);
		page_provLine(buf, len);
		lines++;

		/* Oldest first; seq is the event's ordinal among all events of this page */
		n = (total < C1PROV_DEPTH) ? total : C1PROV_DEPTH;
		for (j = total - n; j < total; j++) {
			e = &ev[j % C1PROV_DEPTH];
			len = lib_sprintf(buf, "C1PROV d=%u.%u pa=0x%llx ev=%s pid=%d tick=%u npages=%u kind=%s head=0x%llx seq=%u age=%u",
				dump, reason, (unsigned long long)pa, page_provEvName[(e->ev < 4U) ? e->ev : 0U], e->pid, e->ms,
				(unsigned int)e->npages, page_provKindName[(e->kind < C1PROV_KINDS) ? e->kind : 0U],
				(unsigned long long)e->head * SIZE_PAGE, j, now - e->ms);
			len += page_provWho(buf + len, e->pid);
			page_provLine(buf, len);
			lines++;
		}
	}

	return lines;
}

#endif /* C1_PAGE_PROVENANCE */
