/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Configuration file for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PH_HAL_CONFIG_H_
#define _PH_HAL_CONFIG_H_

#include <board_config.h>

#define ASID_BITS       16U
/* Pi 4 / BCM2711 has 4 Cortex-A72 cores, but the Pi 4 firmware does
 * NOT reliably bring them all up into the armstub on cold boot
 * (multi-run UART marker capture, 2026-05-23: cpu0 never reaches
 * armstub `in_el2`, secondaries reach it 0-or-1 times per boot —
 * effectively random). Without a PSCI smc handler or a memory-poke
 * wakeup path that targets cpu1/2/3 specifically, Phoenix-RTOS
 * cannot count on the standard armstub spin-table protocol to
 * release secondaries.
 *
 * NUM_CPUS is 4 and all four A72 cores schedule: secondaries pass the
 * hal_smpPrimaryReady gate in _other_core_virtual, bring up their per-CPU
 * state (_set_up_vbar_and_stacks, _hal_interruptsInitPerCPU, _hal_cpuInit,
 * _hal_timerInitPerCPU), unmask DAIF and take timer PPIs, so they run the
 * scheduler alongside cpu0. The cold-boot core-release caveat above still
 * applies: a core the firmware never released into the armstub stays down. */
#define NUM_CPUS        4U
#define SIZE_INTERRUPTS 256U

#ifndef PL011_TTY_BASE
#define PL011_TTY_BASE 0x09000000u
#endif

#ifndef TIMER_IRQ_GROUP
#define TIMER_IRQ_GROUP 1U
#endif

#ifndef __ASSEMBLY__

#if defined(__TARGET_AARCH64A72)
#define HAL_NAME_PLATFORM "AArch64 Cortex-A72 Generic "
#else
#define HAL_NAME_PLATFORM "AArch64 Cortex-A53 Generic "
#endif

#include "include/arch/aarch64/generic/generic.h"
#include "include/arch/aarch64/generic/syspage.h"
#include "include/syspage.h"

#endif

#endif
