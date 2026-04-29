/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Syspage
 *
 * Copyright 2021 Phoenix Systems
 * Authors: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "lib/lib.h"
#include "syspage.h"


static struct {
	/* parasoft-suppress-next-line MISRAC2012-RULE_5_8 "Variable inside the structure so it shouldn't cause this violation" */
	syspage_t *syspage;
} syspage_common;


size_t syspage_mapSize(void)
{
	size_t nb = 0;
	const syspage_map_t *map = syspage_common.syspage->maps;

	if (map == NULL) {
		return nb;
	}

	do {
		++nb;
		map = map->next;
	} while (map != syspage_common.syspage->maps);

	return nb;
}


const syspage_map_t *syspage_mapList(void)
{
	return syspage_common.syspage->maps;
}


const syspage_map_t *syspage_mapIdResolve(unsigned int id)
{
	const syspage_map_t *map = syspage_common.syspage->maps;

	if (map == NULL) {
		return NULL;
	}

	do {
		if (id == map->id) {
			return map;
		}
		map = map->next;
	} while (map != syspage_common.syspage->maps);

	return NULL;
}


const syspage_map_t *syspage_mapAddrResolve(addr_t addr)
{
	const syspage_map_t *map = syspage_common.syspage->maps;

	if (map == NULL) {
		return NULL;
	}

	do {
		if (addr < map->end && addr >= map->start) {
			return map;
		}
		map = map->next;
	} while (map != syspage_common.syspage->maps);

	return NULL;
}


const syspage_map_t *syspage_mapNameResolve(const char *name)
{
	const syspage_map_t *map = syspage_common.syspage->maps;

	if (map == NULL) {
		return NULL;
	}

	do {
		if (hal_strcmp(name, map->name) == 0) {
			return map;
		}
		map = map->next;
	} while (map != syspage_common.syspage->maps);

	return NULL;
}


size_t syspage_progSize(void)
{
	size_t nb = 0;
	const syspage_prog_t *prog = syspage_common.syspage->progs;

	if (prog == NULL) {
		return nb;
	}

	do {
		++nb;
		prog = prog->next;
	} while (prog != syspage_common.syspage->progs);

	return nb;
}


syspage_prog_t *syspage_progList(void)
{
	return syspage_common.syspage->progs;
}


const syspage_prog_t *syspage_progIdResolve(unsigned int id)
{
	unsigned int i = 0;
	const syspage_prog_t *prog = syspage_common.syspage->progs;

	if (prog == NULL) {
		return NULL;
	}

	do {
		if (id == i++) {
			return prog;
		}
		prog = prog->next;
	} while (prog != syspage_common.syspage->progs);

	return NULL;
}


const syspage_prog_t *syspage_progNameResolve(const char *name)
{
	const syspage_prog_t *prog = syspage_common.syspage->progs;

	if (prog == NULL) {
		return NULL;
	}

	do {
		if (hal_strcmp(name, prog->argv) == 0) {
			return prog;
		}
		prog = prog->next;
	} while (prog != syspage_common.syspage->progs);

	return NULL;
}


void syspage_progShow(void)
{
	const char *name;
	const syspage_prog_t *prog = syspage_common.syspage->progs, *next;

	if (prog != NULL) {
		do {
			name = (prog->argv[0] == 'X') ? prog->argv + 1 : prog->argv;
			next = prog->next;
			lib_printf(" '%s'%c", name, (next == syspage_common.syspage->progs) ? '\n' : ',');
			prog = next;
		} while (prog != syspage_common.syspage->progs);
	}
}


void syspage_init(void)
{
	syspage_prog_t *prog;
	syspage_map_t *map;
	mapent_t *entry;

	/* DEBUG: Send marker to confirm syspage_init entry.
	 * Use PL011_TTY_EARLY_VADDR (TTBR1-mapped) to match _init.S's uart_putc_virt macro.
	 * The physical address 0xfe201000 is not mapped in TTBR1 and only appeared to work
	 * via stale TLB entries from the pre-MMU identity map — producing non-deterministic
	 * marker output between runs. */
	volatile unsigned int *uart = (volatile unsigned int *)0xffffffffffe00000ull;
	volatile unsigned int *uartfr = (volatile unsigned int *)0xffffffffffe00018ull;
	while (*uartfr & 0x20) {}
	*uart = 'F'; /* F marker - syspage_init entered */

	/* TD-04 hang-localization probe: a recent regression has the kernel
	 * stopping between 'F' and 'G'. Pin down whether the call sequence
	 * itself, hal_syspageAddr() return, or the syspage_common.syspage
	 * write is the failing step. */
	while (*uartfr & 0x20) {}
	*uart = '1';
	{
		syspage_t *tmp = (syspage_t *)hal_syspageAddr();
		while (*uartfr & 0x20) {}
		*uart = '2';
		syspage_common.syspage = tmp;
	}
	while (*uartfr & 0x20) {}
	*uart = '3';

	/* DEBUG: Send marker after hal_syspageAddr call */
	while (*uartfr & 0x20) {}
	*uart = 'G'; /* G marker - after hal_syspageAddr */

	/* DEBUG: Dump top 8 hex nibbles (bits[63:32]) of:
	 *   - syspage_common.syspage  (expect 0xffffffff if reloc'd VA)
	 *   - relOffs                 (expect 0xffffffff if huge delta)
	 *   - syspage_common.syspage->maps raw value (expect 0x00000000 pre-reloc — it's plo PA)
	 * Framed by 'P{' ... '}' and '{' ... '}' pairs so the three 8-hex dumps
	 * are easy to pick out of the UART stream. */
	{
		extern u64 relOffs;
		unsigned long long v;
		int shift;
		int i;
		unsigned long long vals[3];
		const char tags[3] = { 'p', 'r', 'q' }; /* syspage ptr, relOffs, maps(raw) */
		while (*uartfr & 0x20) {}
		*uart = 'H'; /* H marker - before reading syspage vals */
		vals[0] = (unsigned long long)(unsigned long)syspage_common.syspage;
		while (*uartfr & 0x20) {}
		*uart = 'I'; /* I marker - about to read relOffs */
		vals[1] = (unsigned long long)relOffs;
		while (*uartfr & 0x20) {}
		*uart = 'J'; /* J marker - about to deref syspage->maps */
		vals[2] = (unsigned long long)(unsigned long)syspage_common.syspage->maps;
		while (*uartfr & 0x20) {}
		*uart = 'K'; /* K marker - after reading syspage->maps */
		/* Dump plo-reported syspage size (low 32 bits) to check whether it
		 * exceeds the 4 KiB _hal_syspageCopied buffer — that would explain
		 * garbage entry pointers seen beyond the first ~7 iterations. */
		{
			unsigned long long sv = (unsigned long long)syspage_common.syspage->size;
			int sshift;
			while (*uartfr & 0x20) {}
			*uart = 's';
			while (*uartfr & 0x20) {}
			*uart = '{';
			for (sshift = 28; sshift >= 0; sshift -= 4) {
				unsigned int n = (unsigned int)((sv >> sshift) & 0xfU);
				while (*uartfr & 0x20) {}
				*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
			}
			while (*uartfr & 0x20) {}
			*uart = '}';
		}
		for (i = 0; i < 3; i++) {
			while (*uartfr & 0x20) {}
			*uart = tags[i];
			while (*uartfr & 0x20) {}
			*uart = '{';
			v = vals[i];
			for (shift = 60; shift >= 32; shift -= 4) {
				unsigned int n = (unsigned int)((v >> shift) & 0xfU);
				while (*uartfr & 0x20) {}
				*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
			}
			while (*uartfr & 0x20) {}
			*uart = '}';
		}
	}

	/* DEBUG: Check if syspage is NULL and send marker */
	if (syspage_common.syspage == NULL) {
		while (*uartfr & 0x20) {}
		*uart = 'N'; /* N marker - syspage is NULL! */
	}
	else {
		while (*uartfr & 0x20) {}
		*uart = 'V'; /* V marker - syspage is valid */
	}

	/* DEBUG: Send marker before accessing syspage->maps */
	while (*uartfr & 0x20) {}
	*uart = 'W'; /* W marker - about to access syspage->maps */

	/* Map's relocation */
	if (syspage_common.syspage->maps != NULL) {
		while (*uartfr & 0x20) {}
		*uart = 'X'; /* X marker - syspage->maps is not NULL */
		syspage_common.syspage->maps = hal_syspageRelocate(syspage_common.syspage->maps);
		while (*uartfr & 0x20) {}
		*uart = 'a'; /* a marker - after maps relocation */
		map = syspage_common.syspage->maps;
		do {
			while (*uartfr & 0x20) {}
			*uart = 'b'; /* b marker - in map loop */
			map->next = hal_syspageRelocate(map->next);
			while (*uartfr & 0x20) {}
			*uart = 'c'; /* c marker - after next relocation */
			map->prev = hal_syspageRelocate(map->prev);
			while (*uartfr & 0x20) {}
			*uart = 'd'; /* d marker - after prev relocation */
			map->name = hal_syspageRelocate(map->name);
			while (*uartfr & 0x20) {}
			*uart = 'e'; /* e marker - after name relocation */

			if (map->entries != NULL) {
				while (*uartfr & 0x20) {}
				*uart = 'f'; /* f marker - entries not NULL */
				map->entries = hal_syspageRelocate(map->entries);
				while (*uartfr & 0x20) {}
				*uart = 'g'; /* g marker - after entries relocation */

				/* Raw-byte dump of copied syspage at the suspected-corrupt offset
				 * range 0x310..0x32f. Framed as B{hh hh hh ...}. If these bytes
				 * are nondeterministic across runs, the copy itself is not
				 * faithful or plo never wrote them. If they are deterministic
				 * but nonsensical as mapent_t, the chain logic is at fault. */
				{
					const volatile unsigned char *base =
						(const volatile unsigned char *)syspage_common.syspage;
					int boff;
					while (*uartfr & 0x20) {}
					*uart = 'B';
					while (*uartfr & 0x20) {}
					*uart = '{';
					for (boff = 0x310; boff < 0x330; boff++) {
						unsigned int byte = base[boff];
						unsigned int hi = (byte >> 4) & 0xfU;
						unsigned int lo = byte & 0xfU;
						while (*uartfr & 0x20) {}
						*uart = (hi < 10U) ? ('0' + hi) : ('a' + hi - 10U);
						while (*uartfr & 0x20) {}
						*uart = (lo < 10U) ? ('0' + lo) : ('a' + lo - 10U);
					}
					while (*uartfr & 0x20) {}
					*uart = '}';
				}
				entry = map->entries;
				/* Snapshot the relocated head so the circular-list terminator
				 * is still readable after we walk the list. Previously the
				 * code relocated map->entries a second time here, which left
				 * the field pointing to a bogus VA — harmless for this loop
				 * (which uses the local snapshot), removed for clarity. */
				mapent_t *original_entries = map->entries;

				/* Dump the tail (head->prev resolved) low 32 bits as T{...}.
				 * head->prev is still a raw plo PA here (not yet walked), so
				 * hal_syspageRelocate turns it into the high VA of the tail
				 * entry. Comparing this against walked entries tells us how
				 * many entries plo actually put in the list. */
				{
					mapent_t *tail = hal_syspageRelocate(original_entries->prev);
					unsigned long long tv = (unsigned long long)(unsigned long)tail;
					int tshift;
					while (*uartfr & 0x20) {}
					*uart = 'T';
					while (*uartfr & 0x20) {}
					*uart = '{';
					for (tshift = 28; tshift >= 0; tshift -= 4) {
						unsigned int n = (unsigned int)((tv >> tshift) & 0xfU);
						while (*uartfr & 0x20) {}
						*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
					}
					while (*uartfr & 0x20) {}
					*uart = '}';
				}

				/* TD-04 diagnostic: hard cap on iterations so a missing
				 * terminator (or an oversized syspage overrunning the
				 * _hal_syspageCopied buffer) produces a distinct marker
				 * instead of an infinite/faulting loop. Also emit one hex
				 * nibble of entry[55:52] before dereferencing, so a pointer
				 * corrupted out of the high-canonical kernel range is
				 * visible on UART instead of just faulting silently. */
				int entryCount = 0;
				/* Dump original_entries low 32 bits once, tag 'O{...}', so we
				 * can compare against each iteration's entry low 32 bits. */
				{
					unsigned long long oe = (unsigned long long)(unsigned long)original_entries;
					int oshift;
					while (*uartfr & 0x20) {}
					*uart = 'O';
					while (*uartfr & 0x20) {}
					*uart = '{';
					for (oshift = 28; oshift >= 0; oshift -= 4) {
						unsigned int n = (unsigned int)((oe >> oshift) & 0xfU);
						while (*uartfr & 0x20) {}
						*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
					}
					while (*uartfr & 0x20) {}
					*uart = '}';
				}
				do {
					while (*uartfr & 0x20) {}
					*uart = 'h'; /* h marker - in entry loop */
					/* Dump low 32 bits of entry as 8 hex digits, framed {...}.
					 * Combined with tag 'O{...}' above, reveals whether entry
					 * equals original_entries or is a stray pointer. */
					{
						unsigned long long ev = (unsigned long long)(unsigned long)entry;
						int eshift;
						while (*uartfr & 0x20) {}
						*uart = '{';
						for (eshift = 28; eshift >= 0; eshift -= 4) {
							unsigned int n = (unsigned int)((ev >> eshift) & 0xfU);
							while (*uartfr & 0x20) {}
							*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
						}
						while (*uartfr & 0x20) {}
						*uart = '}';
					}
					/* Safety cap: with the TD-04 NC-dest fix in place the
					 * entry list walks cleanly with no spurious garbage
					 * pointers, but plo can legitimately emit > 10 entries
					 * per map (observed: 11 on Pi 4 first map). 64 is a
					 * wide margin over any plausible map population while
					 * still bounding the loop against pathological inputs. */
					if (entryCount++ >= 64) {
						while (*uartfr & 0x20) {}
						*uart = 'Q'; /* Q marker - entry-loop safety cap hit */
						break;
					}
					entry->next = hal_syspageRelocate(entry->next);
					while (*uartfr & 0x20) {}
					*uart = 'i'; /* i marker - after entry next relocation */
					entry->prev = hal_syspageRelocate(entry->prev);
					while (*uartfr & 0x20) {}
					*uart = 'j'; /* j marker - after entry prev relocation */
					/* Dump the just-relocated prev low 32 as R{...}. Combined
					 * with h{...} (entry) and T{...} (tail), we get the full
					 * doubly-linked chain view. */
					{
						unsigned long long pv = (unsigned long long)(unsigned long)entry->prev;
						int pshift;
						while (*uartfr & 0x20) {}
						*uart = 'R';
						while (*uartfr & 0x20) {}
						*uart = '{';
						for (pshift = 28; pshift >= 0; pshift -= 4) {
							unsigned int n = (unsigned int)((pv >> pshift) & 0xfU);
							while (*uartfr & 0x20) {}
							*uart = (n < 10U) ? ('0' + n) : ('a' + n - 10U);
						}
						while (*uartfr & 0x20) {}
						*uart = '}';
					}
					while (*uartfr & 0x20) {}
					*uart = 'k'; /* k marker - before entry next assignment */
					entry = entry->next;
					while (*uartfr & 0x20) {}
					*uart = 'l'; /* l marker - after entry next assignment */
				} while (entry != original_entries);
			}
			while (*uartfr & 0x20) {}
			*uart = 'l'; /* l marker - before map next */
			map = map->next;
			while (*uartfr & 0x20) {}
			*uart = 'm'; /* m marker - after map next */
		} while (map != syspage_common.syspage->maps);
		while (*uartfr & 0x20) {}
		*uart = 'n'; /* n marker - end of map loop */
	}

	/* Program's relocation */
	if (syspage_common.syspage->progs != NULL) {
		syspage_common.syspage->progs = hal_syspageRelocate(syspage_common.syspage->progs);
		prog = syspage_common.syspage->progs;

		do {
			prog->next = hal_syspageRelocate(prog->next);
			prog->prev = hal_syspageRelocate(prog->prev);

			prog->dmaps = hal_syspageRelocate(prog->dmaps);
			prog->imaps = hal_syspageRelocate(prog->imaps);
			prog->argv = hal_syspageRelocate(prog->argv);
			prog = prog->next;
		} while (prog != syspage_common.syspage->progs);
	}

	/* DEBUG: Send marker at end of syspage_init() */
	while (*uartfr & 0x20) {}
	*uart = 'Y'; /* Y marker - syspage_init() completed successfully */
}
