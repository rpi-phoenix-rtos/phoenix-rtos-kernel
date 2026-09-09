/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Fine-grained memory allocator
 *
 * Copyright 2012, 2017 Phoenix Systems
 * Copyright 2001, 2005-2006 Pawel Pisarczyk
 * Author: Pawel Pisarczyk
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hal/hal.h"
#include "lib/lib.h"
#include "map.h"
#include "zone.h"
#include "include/errno.h"
#include "proc/proc.h"


static struct {
	vm_zone_t *sizes[24];
	vm_zone_t *used;
	vm_zone_t firstzone;

	rbtree_t tree;

	unsigned int hdrblocks;
	size_t allocsz;

	unsigned int zonehdrs;
	lock_t lock;
} kmalloc_common;


static int kmalloc_zone_cmp(rbnode_t *n1, rbnode_t *n2)
{
	vm_zone_t *z1 = lib_treeof(vm_zone_t, linkage, n1);
	vm_zone_t *z2 = lib_treeof(vm_zone_t, linkage, n2);

	/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "Variable pass to lib_treeof will not be NULL, so lib_treeof will not be NULL either" */
	if ((ptr_t)z1->vaddr > (ptr_t)z2->vaddr) {
		return 1;
	}

	if (((ptr_t)z2->vaddr >= (ptr_t)z1->vaddr) && ((ptr_t)z2->vaddr < (ptr_t)z1->vaddr + z1->blocks * z1->blocksz)) {
		return 0;
	}
	if (((ptr_t)z1->vaddr >= (ptr_t)z2->vaddr) && ((ptr_t)z1->vaddr < (ptr_t)z2->vaddr + z2->blocks * z2->blocksz)) {
		return 0;
	}

	return -1;
}


/* Validate a zone's list links BEFORE LIST_REMOVE dereferences them.
 *
 * This is not a theoretical guard. On 2026-09-09 a zone reached _kmalloc_free
 * with next = 0x80000001c46ccf80 where a kernel pointer must be
 * 0xffffffffc46ccf80 -- the low 32 bits intact, the high 32 bits replaced -- and
 * lib_listRemove's `t->next->prev = t->prev` (lib/list.c:47) took a translation
 * fault level 0 at EL1, on a non-canonical address. The board died there.
 *
 * A range test is deliberately used rather than an aarch64 canonicality test
 * ((p >> 48) != 0xffff): VADDR_KERNEL is defined by every HAL, so this keeps
 * working on ia32/armv7a/riscv64/sparcv8leon, and requiring the link to be in
 * KERNEL space is strictly stronger than requiring it to be canonical. It also
 * catches a NULL link, which would fault the same way.
 *
 * Note what this does NOT try to be: an integrity check. It is a cheap value
 * test on the two pointers about to be dereferenced. lib_listBelongs() is not
 * usable here and the note that proposed it was wrong -- it discards `poff`
 * (lib/list.c:61) and returns at lib/list.c:69 as soon as the walk reaches the
 * node, i.e. *before* ever reading node->next, so for a zone that is genuinely
 * on the used list (which this one was: the `t->prev->next` store on line 46
 * committed) it returns 1 and the fault is byte-identical. Worse, its walk would
 * itself dereference the already-poisoned predecessor link.
 *
 * Returns 1 if both links are plausible, 0 if either is not.
 */
static int kmalloc_zoneLinksSane(const vm_zone_t *z)
{
	if (((addr_t)z->next < (addr_t)VADDR_KERNEL) || ((addr_t)z->prev < (addr_t)VADDR_KERNEL)) {
		return 0;
	}

	return 1;
}


/* Name the victim. The abort this replaces gave only a register dump, from which
 * it took a disassembly of lib_listRemove to work out even WHICH field was
 * corrupt (it was next; an earlier write-up said prev). Printing the zone's own
 * state says what the block was being used for, which the register dump cannot.
 *
 * ⚠ lib_printf MUST NOT be used here, and this is not a style preference. Both
 * callers hold kmalloc_common.lock (a NON-recursive mutex, taken at vm_kmalloc
 * and vm_kfree), and lib_printf -> lib_putch -> log_write reaches
 * vm_kmalloc(sizeof(*rmsg)) at log/log.c:300 -- so printing here would re-enter
 * the allocator lock and hang the kernel on the exact path this guard exists to
 * make survivable. hal_consolePrint takes only console_common.lock, which the
 * HAL documents as a leaf lock (hal/aarch64/generic/console.c:80) and which
 * allocates nothing, so it is safe under any kernel lock.
 *
 * The static buffer is safe for the same reason the guard is needed: both call
 * sites hold kmalloc_common.lock, so they cannot interleave. */
static char kmalloc_diagBuf[224];


static void kmalloc_reportBadZone(const vm_zone_t *z, u8 idx)
{
	(void)lib_sprintf(kmalloc_diagBuf,
			"kmalloc: zone %p has a corrupt list link -- next=%p prev=%p "
			"(used=%u blocks=%u blocksz=%zu vaddr=%p idx=%u); zone stranded, not unlinked\n",
			(const void *)z, (void *)z->next, (void *)z->prev, z->used, z->blocks,
			z->blocksz, z->vaddr, (unsigned int)idx);
	hal_consolePrint(ATTR_BOLD, kmalloc_diagBuf);
}


static void *_kmalloc_alloc(u8 hdridx, u8 idx)
{
	void *b;
	vm_zone_t *z = kmalloc_common.sizes[idx];

	b = _vm_zalloc(z, NULL);
	if (b != NULL) {
		kmalloc_common.allocsz += (1UL << idx);

		if (idx == hdridx) {
			kmalloc_common.hdrblocks--;
		}

		if (z->used == z->blocks) {
			/* Same guard as in _kmalloc_free: this LIST_REMOVE dereferences the
			 * same two pointers, so it can fault the same way. */
			if (kmalloc_zoneLinksSane(z) == 0) {
				kmalloc_reportBadZone(z, idx);
			}
			else {
				LIST_REMOVE(&kmalloc_common.sizes[idx], z);
				LIST_ADD(&kmalloc_common.used, z);
			}
		}
	}

	return b;
}


static vm_zone_t *_kmalloc_free(u8 hdridx, void *p)
{
	vm_zone_t t;
	vm_zone_t *z;
	u8 idx;

	/* Free block */
	t.vaddr = p;
	t.blocks = 1;
	t.blocksz = 16;

	z = lib_treeof(vm_zone_t, linkage, lib_rbFind(&kmalloc_common.tree, &t.linkage));
	if (z == NULL) {
		return NULL;
	}

	/* A rejected free (pointer outside the zone or not block-aligned) must not
	 * move the accounting, or allocsz/hdrblocks and the used-list drift away
	 * from the zone's actual state. */
	if (_vm_zfree(z, p) != EOK) {
		return NULL;
	}

	kmalloc_common.allocsz -= z->blocksz;

	idx = (u8)hal_cpuGetLastBit(z->blocksz);
	if (idx == hdridx) {
		kmalloc_common.hdrblocks++;
	}

	/* Remove zone from used list */
	if (z->used == z->blocks - 1U) {
		if (kmalloc_zoneLinksSane(z) == 0) {
			/* Refuse the move rather than fault. Leaving z on the used list
			 * strands one zone (its free block is not reused); unlinking it
			 * through a garbage pointer takes the kernel down, and skipping
			 * only the REMOVE would leave it on two lists at once. */
			kmalloc_reportBadZone(z, idx);
		}
		else {
			LIST_REMOVE(&kmalloc_common.used, z);
			LIST_ADD(&kmalloc_common.sizes[idx], z);
		}
	}

	return z;
}


static int _kmalloc_addZone(u8 hdridx, u8 idx)
{
	vm_zone_t *nz;
	size_t blocksz;
	unsigned int blocks;

	nz = _kmalloc_alloc(hdridx, hdridx);
	if (nz == NULL) {
		return -ENOMEM;
	}

	/* Add new zone */
	blocksz = 0x1UL << idx;
	blocks = (unsigned int)max(((idx == hdridx) ? kmalloc_common.zonehdrs : 1UL), SIZE_PAGE / blocksz);
	if (_vm_zoneCreate(nz, blocksz, blocks) < 0) {
		(void)_kmalloc_free(hdridx, nz);
		return -ENOMEM;
	}

	LIST_ADD(&kmalloc_common.sizes[idx], nz);
	(void)lib_rbInsert(&kmalloc_common.tree, &nz->linkage);

	if (idx == hdridx) {
		kmalloc_common.hdrblocks += nz->blocks;
	}

	return EOK;
}


void *vm_kmalloc(size_t size)
{
	u8 idx, hdridx;
	void *b = NULL;
	vm_zone_t *z;
	int err = EOK;

	/* Establish minimal size */
	size = size < 16U ? 16U : size;

	idx = (u8)hal_cpuGetLastBit(size);
	/* parasoft-begin-suppress MISRAC2012-RULE_14_3 "conditional compilation" */
	if ((u8)hal_cpuGetFirstBit(size) < idx) {
		idx++;
	}
	if (idx >= sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *)) {
		return NULL;
	}
	/* parasoft-end-suppress MISRAC2012-RULE_14_3 */

	hdridx = (u8)hal_cpuGetLastBit(sizeof(vm_zone_t));
	if ((u8)hal_cpuGetFirstBit(sizeof(vm_zone_t)) < hdridx) {
		hdridx++;
	}
	if (hdridx >= sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *)) {
		return NULL;
	}

	(void)proc_lockSet(&kmalloc_common.lock);

	/* <= rather than ==: hdrblocks can end up over-reporting the blocks actually
	 * available (a zone quarantined by _vm_zalloc strands its remaining free
	 * blocks while they are still counted here). With an equality test the
	 * count would then never hit 1 again, no header zone would ever be added,
	 * and every later zone creation would fail -- turning a single quarantine
	 * into "vm_kmalloc returns NULL for every new size". */
	if (kmalloc_common.hdrblocks <= 1U) {
		err = _kmalloc_addZone(hdridx, hdridx);
	}

	if (err == 0) {
		z = kmalloc_common.sizes[idx];
		if (z == NULL) {
			err = _kmalloc_addZone(hdridx, idx);
		}
	}

	/* Alloc new fragment */
	if (err == 0) {
		b = _kmalloc_alloc(hdridx, idx);
	}

	(void)proc_lockClear(&kmalloc_common.lock);

	return b;
}


static void *_kmalloc_freeAtom(u8 hdridx, void *p)
{
	vm_zone_t *z;
	u8 idx;

	z = _kmalloc_free(hdridx, p);
	if (z == NULL) {
		return NULL;
	}

	idx = (u8)hal_cpuGetLastBit(z->blocksz);

	/* Remove zone if free.
	 *
	 * The link guard has to cover the whole block, not just the LIST_REMOVE:
	 * destroying the zone while a list still points at it would turn a stranded
	 * zone into a use-after-free, which is worse than the leak. */
	if ((z->used == 0U) && (z != &kmalloc_common.firstzone) && (kmalloc_zoneLinksSane(z) == 0)) {
		kmalloc_reportBadZone(z, idx);
	}
	else if ((z->used == 0U) && (z != &kmalloc_common.firstzone)) {
		LIST_REMOVE(&kmalloc_common.sizes[idx], z);
		(void)_vm_zoneDestroy(z);
		lib_rbRemove(&kmalloc_common.tree, &z->linkage);

		if (idx == hdridx) {
			kmalloc_common.hdrblocks -= z->blocks;
		}
		return z;
	}

	return NULL;
}


void vm_kfree(void *p)
{
	u8 hdridx;

	hdridx = (u8)hal_cpuGetLastBit(sizeof(vm_zone_t));
	/* parasoft-begin-suppress MISRAC2012-RULE_14_3 "conditional compilation" */
	if ((u8)hal_cpuGetFirstBit(sizeof(vm_zone_t)) < hdridx) {
		hdridx++;
	}
	if (hdridx >= sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *)) {
		return;
	}
	/* parasoft-end-suppress MISRAC2012-RULE_14_3 */

	(void)proc_lockSet(&kmalloc_common.lock);

	while (p != NULL) {
		p = _kmalloc_freeAtom(hdridx, p);
	}

	(void)proc_lockClear(&kmalloc_common.lock);
}


void vm_kmallocGetStats(size_t *allocsz)
{
	*allocsz = kmalloc_common.allocsz;
}


void vm_kmallocDump(void)
{
	unsigned int i;
	vm_zone_t *z;

	for (i = 0; i < sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *); i++) {
		lib_printf("sizes[%d]=", i);
		z = kmalloc_common.sizes[i];

		if (z != NULL) {
			do {
				lib_printf("%p(%d/%d) ", z, z->used, z->blocks);
				z = z->next;
			} while (z != kmalloc_common.sizes[i]);
		}

		lib_printf("\n");
	}
}


int _kmalloc_init(void)
{
	unsigned int hdridx, i;
	size_t blocksz;
	unsigned int blocks;

	lib_printf("vm: Initializing kernel memory allocator: ");

	(void)proc_lockInit(&kmalloc_common.lock, &proc_lockAttrDefault, "kmalloc.common");

	hdridx = hal_cpuGetLastBit(sizeof(vm_zone_t));
	if (hal_cpuGetFirstBit(sizeof(vm_zone_t)) < hdridx) {
		hdridx++;
	}
	if (hdridx >= sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *)) {
		lib_printf("BAD HDRIDX!\n");
		return -1;
	}

	/* Initialize sizes */
	for (i = 0; i < sizeof(kmalloc_common.sizes) / sizeof(vm_zone_t *); i++) {
		kmalloc_common.sizes[i] = NULL;
	}
	kmalloc_common.used = NULL;

	/* Initialize allocated zone tree */
	lib_rbInit(&kmalloc_common.tree, kmalloc_zone_cmp, NULL);

	kmalloc_common.zonehdrs = 16;

	/* Add first zone_t zone */
	blocksz = 0x1UL << hdridx;
	blocks = (unsigned int)max(kmalloc_common.zonehdrs, SIZE_PAGE / blocksz);
	(void)_vm_zoneCreate(&kmalloc_common.firstzone, blocksz, blocks);
	LIST_ADD(&kmalloc_common.sizes[hdridx], &kmalloc_common.firstzone);
	(void)lib_rbInsert(&kmalloc_common.tree, &kmalloc_common.firstzone.linkage);

	kmalloc_common.allocsz = 0;
	kmalloc_common.hdrblocks = kmalloc_common.firstzone.blocks;

	lib_printf("(%d*%d) %d\n", kmalloc_common.hdrblocks, sizeof(vm_zone_t), kmalloc_common.hdrblocks * sizeof(vm_zone_t));

	return 0;
}
