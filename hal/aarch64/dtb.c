/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * DTB parser
 *
 * Copyright 2018, 2020, 2024 Phoenix Systems
 * Author: Pawel Pisarczyk, Lukasz Leczkowski, Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "dtb.h"
#include "config.h"
#include "hal/string.h"

#include <arch/pmap.h>
#include <arch/cpu.h>

#include "include/errno.h"


#define STR_AND_LEN(x) (x), (sizeof(x) - 1U)

#define MAX_CPUS       8U
#define MAX_MEM_BANKS  8U
#define MAX_SERIALS    4U
#define MAX_SOC_RANGES 4U

struct _fdt_header_t {
	u32 magic;
	u32 totalsize;
	u32 off_dt_struct;
	u32 off_dt_strings;
	u32 off_mem_rsvmap;
	u32 version;
	u32 last_comp_version;
	u32 boot_cpuid_phys;
	u32 size_dt_strings;
	u32 size_dt_struct;
};


static struct {
	struct _fdt_header_t *fdth;

	void *start;

	char *model;
	char *compatible;
	u32 rootAddressCells;
	u32 rootSizeCells;

	size_t nCpus;
	struct {
		char *compatible;
		u32 clock; /* TODO: on ZynqMP this is not populated */
	} cpus[MAX_CPUS];

	size_t nMemBanks;
	dtb_memBank_t memBanks[MAX_MEM_BANKS];

	struct {
		unsigned int depth;
		u32 addressCells;
		u32 sizeCells;

		size_t nRanges;
		struct {
			addr_t bus;
			addr_t cpu;
			addr_t size;
		} ranges[MAX_SOC_RANGES];
	} soc;

	struct {
		addr_t gicd;
		addr_t gicc;
	} apu_gic;

	dtb_timer_t timer;

	size_t nSerials;
	dtb_serial_t serials[MAX_SERIALS];

	char *stdoutPath;
} dtb_common;


static char *dtb_getString(u32 i)
{
	return (char *)((void *)dtb_common.fdth + ntoh32(dtb_common.fdth->off_dt_strings) + i);
}


static int dtb_readCells(const void *data, u32 cells, addr_t *value)
{
	const u8 *p = data;
	addr_t v = 0;
	u32 cell;
	u32 i;

	if ((data == NULL) || (value == NULL) || (cells == 0U) || (cells > 2U)) {
		return -EINVAL;
	}

	for (i = 0U; i < cells; ++i) {
		hal_memcpy(&cell, p + (i * sizeof(cell)), sizeof(cell));
		v = (v << 32) | ntoh32(cell);
	}

	*value = v;

	return EOK;
}


static int dtb_translateSocAddress(addr_t *addr)
{
	addr_t offset;
	size_t i;

	if (addr == NULL) {
		return -EINVAL;
	}

	for (i = 0U; i < dtb_common.soc.nRanges; ++i) {
		if (*addr < dtb_common.soc.ranges[i].bus) {
			continue;
		}

		offset = *addr - dtb_common.soc.ranges[i].bus;
		if (offset < dtb_common.soc.ranges[i].size) {
			*addr = dtb_common.soc.ranges[i].cpu + offset;
			return EOK;
		}
	}

	return -ENODEV;
}


/* Decodes cells in reg property for GIC-400 interrupt controller into interrupt number */
static int dtb_getIntrFromReg(char *reg)
{
	u32 type, num;
	hal_memcpy(&type, reg, 4);
	type = ntoh32(type);
	hal_memcpy(&num, reg + 4, 4);
	num = ntoh32(num);
	/* Ignore the third cell (flags) - currently we don't have need for it */

	if ((type == 0U) && (num < 988U)) {
		/* Valid SPI interrupt number */
		return (int)num + 32;
	}
	else if ((type == 1U) && (num < 16U)) {
		/* Valid PPI interrupt number */
		return (int)num + 16;
	}
	else {
		/* No action required */
	}

	return -1;
}


static void dtb_parseSystem(void *dtb, u32 si, u32 l)
{
	if (hal_strcmp(dtb_getString(si), "model") == 0) {
		dtb_common.model = dtb;
	}
	else if (hal_strcmp(dtb_getString(si), "compatible") == 0) {
		dtb_common.compatible = dtb;
	}
	else if ((l >= 4U) && (hal_strcmp(dtb_getString(si), "#address-cells") == 0)) {
		dtb_common.rootAddressCells = ntoh32(*(u32 *)dtb);
	}
	else if ((l >= 4U) && (hal_strcmp(dtb_getString(si), "#size-cells") == 0)) {
		dtb_common.rootSizeCells = ntoh32(*(u32 *)dtb);
	}
	else {
		/* No action required */
	}
}


static void dtb_parseSOC(void *dtb, u32 si, u32 l)
{
	addr_t bus, cpu, size;
	u32 tupleCells;

	if ((l >= 4U) && (hal_strcmp(dtb_getString(si), "#address-cells") == 0)) {
		dtb_common.soc.addressCells = ntoh32(*(u32 *)dtb);
		return;
	}

	if ((l >= 4U) && (hal_strcmp(dtb_getString(si), "#size-cells") == 0)) {
		dtb_common.soc.sizeCells = ntoh32(*(u32 *)dtb);
		return;
	}

	if (hal_strcmp(dtb_getString(si), "ranges") != 0) {
		return;
	}

	tupleCells = dtb_common.soc.addressCells + dtb_common.rootAddressCells + dtb_common.soc.sizeCells;
	if ((tupleCells == 0U) || (tupleCells > 5U)) {
		return;
	}

	while ((l >= (tupleCells * sizeof(u32))) && (dtb_common.soc.nRanges < MAX_SOC_RANGES)) {
		if ((dtb_readCells(dtb, dtb_common.soc.addressCells, &bus) < 0) ||
			(dtb_readCells(dtb + (dtb_common.soc.addressCells * sizeof(u32)), dtb_common.rootAddressCells, &cpu) < 0) ||
			(dtb_readCells(dtb + ((dtb_common.soc.addressCells + dtb_common.rootAddressCells) * sizeof(u32)), dtb_common.soc.sizeCells, &size) < 0)) {
			break;
		}

		dtb_common.soc.ranges[dtb_common.soc.nRanges].bus = bus;
		dtb_common.soc.ranges[dtb_common.soc.nRanges].cpu = cpu;
		dtb_common.soc.ranges[dtb_common.soc.nRanges].size = size;
		dtb_common.soc.nRanges++;

		dtb += tupleCells * sizeof(u32);
		l -= tupleCells * sizeof(u32);
	}
}


static void dtb_parseCPU(void *dtb, u32 si, u32 l)
{
	if (hal_strcmp(dtb_getString(si), "compatible") == 0) {
		dtb_common.cpus[dtb_common.nCpus].compatible = dtb;
	}
	else if (hal_strcmp(dtb_getString(si), "clock-frequency") == 0) {
		dtb_common.cpus[dtb_common.nCpus].clock = ntoh32(*(u32 *)dtb);
	}
	else {
		/* No action required */
	}
}


static void dtb_parseInterruptController(void *dtb, u32 si, u32 l, int inSOC)
{
	u64 gicc, gicd;
	u32 giccOffs;
	u32 tupleCells;

	if (hal_strcmp(dtb_getString(si), "reg") == 0) {
		if ((inSOC != 0) && (dtb_common.soc.addressCells != 0U) && (dtb_common.soc.sizeCells != 0U)) {
			tupleCells = dtb_common.soc.addressCells + dtb_common.soc.sizeCells;
			if ((tupleCells != 0U) && (l >= (2U * tupleCells * sizeof(u32)))) {
				if ((dtb_readCells(dtb, dtb_common.soc.addressCells, &dtb_common.apu_gic.gicd) == EOK) &&
					(dtb_readCells(dtb + (tupleCells * sizeof(u32)), dtb_common.soc.addressCells, &dtb_common.apu_gic.gicc) == EOK)) {
					(void)dtb_translateSocAddress(&dtb_common.apu_gic.gicd);
					(void)dtb_translateSocAddress(&dtb_common.apu_gic.gicc);
				}
			}

			return;
		}

		if (l >= 24U) {
			/* The current ZynqMP path uses 12-byte tuples (64-bit address, 32-bit size),
			 * while QEMU virt exposes 16-byte tuples (64-bit address, 64-bit size).
			 */
			giccOffs = (l >= 32U) ? 16U : 12U;

			hal_memcpy(&gicd, dtb + 0, 8);
			gicd = ntoh64(gicd);
			hal_memcpy(&gicc, dtb + giccOffs, 8);
			gicc = ntoh64(gicc);
			dtb_common.apu_gic.gicd = gicd;
			dtb_common.apu_gic.gicc = gicc;
		}
	}
}


static void dtb_parseSerial(void *dtb, u32 si, u32 l, int inSOC)
{
	u64 base;

	if (hal_strcmp(dtb_getString(si), "reg") == 0) {
		if ((inSOC != 0) && (dtb_common.soc.addressCells != 0U) && (l >= (dtb_common.soc.addressCells * sizeof(u32)))) {
			if (dtb_readCells(dtb, dtb_common.soc.addressCells, &dtb_common.serials[dtb_common.nSerials].base) == EOK) {
				(void)dtb_translateSocAddress(&dtb_common.serials[dtb_common.nSerials].base);
			}
		}
		else if (l >= 8U) {
			hal_memcpy(&base, dtb, 8);
			base = ntoh64(base);
			dtb_common.serials[dtb_common.nSerials].base = base;
		}
	}
	else if (hal_strcmp(dtb_getString(si), "interrupts") == 0) {
		if (l >= 12U) {
			dtb_common.serials[dtb_common.nSerials].intr = dtb_getIntrFromReg(dtb);
		}
	}
	else {
		/* No action required */
	}
}


static void dtb_parseTimer(void *dtb, u32 si, u32 l)
{
	if (hal_strcmp(dtb_getString(si), "interrupts") != 0) {
		return;
	}

	/* ARM architectural timer interrupt order:
	 * secure physical, non-secure physical, virtual, hypervisor.
	 */
	if (l >= 12U) {
		dtb_common.timer.physSecure = dtb_getIntrFromReg(dtb);
	}

	if (l >= 24U) {
		dtb_common.timer.physNonSecure = dtb_getIntrFromReg(dtb + 12);
	}

	if (l >= 36U) {
		dtb_common.timer.virt = dtb_getIntrFromReg(dtb + 24);
	}

	if (l >= 48U) {
		dtb_common.timer.hyp = dtb_getIntrFromReg(dtb + 36);
	}
}


static void dtb_parseChosen(void *dtb, u32 si, u32 l)
{
	if ((l == 0U) || (hal_strcmp(dtb_getString(si), "stdout-path") != 0)) {
		return;
	}

	dtb_common.stdoutPath = dtb;
}


static int dtb_parseMemory(void *dtb, u32 si, u32 l)
{
	addr_t start = 0, size = 0;
	u32 tupleCells;
	if (hal_strcmp(dtb_getString(si), "reg") == 0) {
		tupleCells = dtb_common.rootAddressCells + dtb_common.rootSizeCells;
		if ((tupleCells == 0U) || (tupleCells > 4U)) {
			return 0;
		}

		while ((l >= (tupleCells * sizeof(u32))) && (dtb_common.nMemBanks < MAX_MEM_BANKS)) {
			if ((dtb_readCells(dtb, dtb_common.rootAddressCells, &start) < 0) ||
				(dtb_readCells(dtb + (dtb_common.rootAddressCells * sizeof(u32)), dtb_common.rootSizeCells, &size) < 0)) {
				break;
			}

			if (size != 0U) {
				dtb_common.memBanks[dtb_common.nMemBanks].start = start;
				dtb_common.memBanks[dtb_common.nMemBanks].end = start + size - 1U;
				dtb_common.nMemBanks++;
			}

			l -= tupleCells * sizeof(u32);
			dtb += tupleCells * sizeof(u32);
		}
	}
	return 0;
}


static int dtb_isInterruptControllerNode(const char *nodeName, unsigned int depth, int inAMBA_APU)
{
	if ((inAMBA_APU != 0) && (hal_strncmp(nodeName, STR_AND_LEN("interrupt-controller@")) == 0)) {
		return 1;
	}

	if ((depth <= 2U) && (hal_strncmp(nodeName, STR_AND_LEN("interrupt-controller@")) == 0)) {
		return 1;
	}

	if ((depth <= 2U) && (hal_strncmp(nodeName, STR_AND_LEN("intc@")) == 0)) {
		return 1;
	}

	return 0;
}


static int dtb_isSerialNode(const char *nodeName, unsigned int depth)
{
	if ((depth == 2U) && (hal_strncmp(nodeName, STR_AND_LEN("serial@")) == 0)) {
		return 1;
	}

	if ((depth <= 2U) && (hal_strncmp(nodeName, STR_AND_LEN("pl011@")) == 0)) {
		return 1;
	}

	return 0;
}


static int dtb_chooseTimerSource(dtb_timerSource_t *source, int *intr)
{
	/* Keep the first common EL1 policy explicit:
	 * prefer the virtual timer, then fall back to the non-secure physical timer.
	 */
	if ((source == NULL) || (intr == NULL)) {
		return -EINVAL;
	}

	*source = dtb_timerNone;
	*intr = -1;

#ifdef DTB_FORCE_PHYS_TIMER
	if (dtb_common.timer.physNonSecure >= 0) {
		*source = dtb_timerPhysNonSecure;
		*intr = dtb_common.timer.physNonSecure;
		return EOK;
	}

	if (dtb_common.timer.virt >= 0) {
		*source = dtb_timerVirt;
		*intr = dtb_common.timer.virt;
		return EOK;
	}
#else
	if (dtb_common.timer.virt >= 0) {
		*source = dtb_timerVirt;
		*intr = dtb_common.timer.virt;
		return EOK;
	}

	if (dtb_common.timer.physNonSecure >= 0) {
		*source = dtb_timerPhysNonSecure;
		*intr = dtb_common.timer.physNonSecure;
		return EOK;
	}
#endif

	return -ENODEV;
}


static int dtb_getStdoutBase(addr_t *base)
{
	char *path, *end, *mark;
	addr_t value = 0;
	char c;

	if (base == NULL) {
		return -EINVAL;
	}

	path = dtb_common.stdoutPath;
	if (path == NULL) {
		return -ENODEV;
	}

	end = path;
	while ((*end != '\0') && (*end != ':')) {
		++end;
	}

	mark = end;
	while ((mark != path) && (*(mark - 1) != '@')) {
		--mark;
	}

	if ((mark == path) && (*mark != '@')) {
		return -ENODEV;
	}

	while (mark < end) {
		c = *mark++;

		value <<= 4;
		if ((c >= '0') && (c <= '9')) {
			value |= (addr_t)(c - '0');
		}
		else if ((c >= 'a') && (c <= 'f')) {
			value |= (addr_t)(c - 'a' + 10);
		}
		else if ((c >= 'A') && (c <= 'F')) {
			value |= (addr_t)(c - 'A' + 10);
		}
		else {
			return -EINVAL;
		}
	}

	*base = value;
	(void)dtb_translateSocAddress(base);

	return EOK;
}


static void dtb_parse(void)
{
	void *dtb;
	unsigned int depth = 0;
	u32 token, si;
	u32 l;
	enum {
		stateIdle,
		stateSystem,
		stateCPU,
		stateAMBA_APU,
		stateInterruptController,
		stateMemory,
		stateTimer,
		stateChosen,
		stateSerial,
	} state = stateIdle;

	if (dtb_common.fdth->magic != ntoh32(0xd00dfeedU)) {
		return;
	}

	dtb = (void *)dtb_common.fdth + ntoh32(dtb_common.fdth->off_dt_struct);

	for (;;) {
		token = ntoh32(*(u32 *)dtb);
		dtb += 4;

		/* FDT_NODE_BEGIN */
		if (token == 1U) {
			if ((depth == 0U) && (*(char *)dtb == '\0')) {
				state = stateSystem;
			}
			else if ((depth == 1U) && (hal_strncmp(dtb, STR_AND_LEN("memory")) == 0)) {
				state = stateMemory;
			}
			else if ((depth == 1U) && (hal_strcmp(dtb, "soc") == 0)) {
				state = stateIdle;
				dtb_common.soc.depth = depth + 1U;
			}
			else if ((depth == 1U) && (hal_strncmp(dtb, STR_AND_LEN("timer")) == 0)) {
				state = stateTimer;
			}
			else if ((depth == 1U) && (hal_strncmp(dtb, STR_AND_LEN("chosen")) == 0)) {
				state = stateChosen;
			}
			else if ((depth == 1U) && (hal_strncmp(dtb, STR_AND_LEN("amba_apu")) == 0)) {
				state = stateAMBA_APU;
			}
			else if ((depth == 2U) && ((hal_strncmp(dtb, STR_AND_LEN("cpu")) == 0) || (hal_strncmp(dtb, STR_AND_LEN("apu_cpu")) == 0))) {
				if (dtb_common.nCpus < MAX_CPUS) {
					state = stateCPU;
				}
			}
			else if (dtb_isInterruptControllerNode(dtb, depth, state == stateAMBA_APU) != 0) {
				state = stateInterruptController;
			}
			else if (dtb_isSerialNode(dtb, depth) != 0) {
				if (dtb_common.nSerials < MAX_SERIALS) {
					state = stateSerial;
					dtb_common.serials[dtb_common.nSerials].intr = -1;
				}
			}
			else {
				/* No action required */
			}

			dtb += ((hal_strlen(dtb) + 3U) & ~3U);
			depth++;
		}

		/* FDT_PROP */
		else if (token == 3U) {
			l = ntoh32(*(u32 *)dtb);
			l = ((l + 3U) & ~3U);

			dtb += 4;
			si = ntoh32(*(u32 *)dtb);
			dtb += 4;

			if (depth == dtb_common.soc.depth) {
				dtb_parseSOC(dtb, si, l);
			}

			switch (state) {
				case stateSystem:
					if (depth == 1U) {
						dtb_parseSystem(dtb, si, l);
					}
					break;

				case stateMemory:
					(void)dtb_parseMemory(dtb, si, l);
					break;

				case stateInterruptController:
					dtb_parseInterruptController(dtb, si, l, (dtb_common.soc.depth != 0U) && (depth > dtb_common.soc.depth));
					break;

				case stateCPU:
					dtb_parseCPU(dtb, si, l);
					break;

				case stateTimer:
					dtb_parseTimer(dtb, si, l);
					break;

				case stateChosen:
					dtb_parseChosen(dtb, si, l);
					break;

				case stateSerial:
					dtb_parseSerial(dtb, si, l, (dtb_common.soc.depth != 0U) && (depth > dtb_common.soc.depth));
					break;

				default:
					/* No action required */
					break;
			}

			dtb += l;
		}

		/* FDT_NODE_END */
		else if (token == 2U) {
			switch (state) {
				case stateAMBA_APU:
					state = (depth > 2U) ? stateAMBA_APU : stateIdle;
					break;

				case stateCPU:
					dtb_common.nCpus++;
					state = stateIdle;
					break;

				case stateSerial:
					dtb_common.nSerials++;
					state = stateIdle;
					break;

				default:
					state = stateIdle;
					break;
			}
			/* parasoft-suppress-next-line MISRAC2012-DIR_4_1 "The algorithm is designed not to underflow" */
			if ((dtb_common.soc.depth != 0U) && (depth == dtb_common.soc.depth)) {
				dtb_common.soc.depth = 0U;
			}
			depth--;
		}
		else if (token == 9U) {
			break;
		}
		else {
			/* No action required */
		}
	}
}


void dtb_getSystem(char **model, char **compatible)
{
	*model = dtb_common.model;
	*compatible = dtb_common.compatible;
}


int dtb_getCPU(unsigned int n, char **compatible, u32 *clock)
{
	if (n >= dtb_common.nCpus) {
		return -EINVAL;
	}

	*compatible = dtb_common.cpus[n].compatible;
	*clock = dtb_common.cpus[n].clock;

	return EOK;
}


void dtb_getMemory(dtb_memBank_t **banks, size_t *nBanks)
{
	*banks = dtb_common.memBanks;
	*nBanks = dtb_common.nMemBanks;
}


void dtb_getGIC(addr_t *gicc, addr_t *gicd)
{
	*gicc = dtb_common.apu_gic.gicc;
	*gicd = dtb_common.apu_gic.gicd;
}


void dtb_getSerials(dtb_serial_t **serials, size_t *nSerials)
{
	*serials = dtb_common.serials;
	*nSerials = dtb_common.nSerials;
}


int dtb_getConsoleSerial(dtb_serial_t *serial)
{
	addr_t base;
	size_t i;
	int err;

	if (serial == NULL) {
		return -EINVAL;
	}

	err = dtb_getStdoutBase(&base);
	if (err == EOK) {
		for (i = 0U; i < dtb_common.nSerials; ++i) {
			if (dtb_common.serials[i].base == base) {
				*serial = dtb_common.serials[i];
				return EOK;
			}
		}
	}

	if (dtb_common.nSerials == 0U) {
		return -ENODEV;
	}

	*serial = dtb_common.serials[0];

	return EOK;
}


void dtb_getTimer(dtb_timer_t *timer)
{
	*timer = dtb_common.timer;
}


int dtb_getTimerSource(dtb_timerSource_t *source, int *intr)
{
	return dtb_chooseTimerSource(source, intr);
}


void _dtb_init(addr_t dtbPhys)
{
	hal_memset(&dtb_common, 0, sizeof(dtb_common));
	dtb_common.fdth = (void *)((dtbPhys & (SIZE_PAGE - 1U)) + VADDR_DTB);
	dtb_common.rootAddressCells = 2U;
	dtb_common.rootSizeCells = 1U;
	dtb_common.soc.addressCells = 2U;
	dtb_common.soc.sizeCells = 1U;
	dtb_common.timer.physSecure = -1;
	dtb_common.timer.physNonSecure = -1;
	dtb_common.timer.virt = -1;
	dtb_common.timer.hyp = -1;

	dtb_parse();
}
