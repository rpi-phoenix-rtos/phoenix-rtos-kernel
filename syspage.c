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

	/* DEBUG: Send marker to confirm syspage_init entry */
	volatile unsigned int *uart = (volatile unsigned int *)0xfe201000;
	volatile unsigned int *uartfr = (volatile unsigned int *)0xfe201018;
	while (*uartfr & 0x20) {}
	*uart = 'F'; /* F marker - syspage_init entered */

	syspage_common.syspage = (syspage_t *)hal_syspageAddr();

	/* DEBUG: Send marker after hal_syspageAddr call */
	while (*uartfr & 0x20) {}
	*uart = 'G'; /* G marker - after hal_syspageAddr */

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
				entry = map->entries;
				/* FIX: Store original map->entries before relocation to avoid infinite loop */
				mapent_t *original_entries = map->entries;
				map->entries = hal_syspageRelocate(map->entries);
				
				do {
					while (*uartfr & 0x20) {}
					*uart = 'h'; /* h marker - in entry loop */
					entry->next = hal_syspageRelocate(entry->next);
					while (*uartfr & 0x20) {}
					*uart = 'i'; /* i marker - after entry next relocation */
					entry->prev = hal_syspageRelocate(entry->prev);
					while (*uartfr & 0x20) {}
					*uart = 'j'; /* j marker - after entry prev relocation */
					while (*uartfr & 0x20) {}
					*uart = 'k'; /* k marker - before entry next assignment */
					entry = entry->next;
					while (*uartfr & 0x20) {}
					*uart = 'l'; /* l marker - after entry next assignment */
				} while (entry != original_entries);
				while (*uartfr & 0x20) {}
				*uart = 'k'; /* k marker - end of entry loop */
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
