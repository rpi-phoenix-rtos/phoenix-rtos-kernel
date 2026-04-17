/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Hardware Abstraction Layer (AArch64)
 *
 * Copyright 2014, 2018, 2024 Phoenix Systems
 * Author: Pawel Pisarczyk, Jacek Maksymowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/hal.h"
#include "dtb.h"

#include "syspage.h"
#include "halsyspage.h"
#include "arch/pmap.h"

#include "lib/assert.h"

#include <board_config.h>

static struct {
	int started;
} hal_common;


#if defined(PLO_RPI_LED_DIAG) && (PLO_RPI_LED_DIAG != 0) && defined(PLO_RPI_GPIO_BASE_ADDRESS) && (PLO_RPI_GPIO_BASE_ADDRESS != 0)
enum {
	hal_gpio_gpfsel4 = 0x10 / sizeof(u32),
	hal_gpio_gpset1 = 0x20 / sizeof(u32),
	hal_gpio_gpclr1 = 0x2c / sizeof(u32),
	hal_gpio42_shift = 6u,
	hal_gpio42_mask = 1u << 10,
	hal_led_delay_loops = 25000000u,
};


static struct {
	volatile u32 *gpio;
	int gpioReady;
} hal_diag;


static void hal_rpiDiagDelay(unsigned int loops)
{
	volatile unsigned int i;

	for (i = 0u; i < loops; ++i) {
		__asm__ volatile ("nop");
	}
}


static int hal_rpiDiagInit(void)
{
	u32 val;

	if (hal_diag.gpioReady != 0) {
		return 0;
	}

	hal_diag.gpio = _pmap_halMapDevice(PLO_RPI_GPIO_BASE_ADDRESS, 0, SIZE_PAGE);
	if (hal_diag.gpio == NULL) {
		return -1;
	}

	val = hal_diag.gpio[hal_gpio_gpfsel4];
	val &= ~(7u << hal_gpio42_shift);
	val |= 1u << hal_gpio42_shift;
	hal_diag.gpio[hal_gpio_gpfsel4] = val;
	hal_diag.gpio[hal_gpio_gpclr1] = hal_gpio42_mask;
	hal_diag.gpioReady = 1;

	return 0;
}


void hal_rpiDiagPulse(unsigned int stage)
{
	unsigned int i;

	if ((stage == 0u) || (hal_rpiDiagInit() < 0)) {
		return;
	}

	hal_rpiDiagDelay(hal_led_delay_loops * 3u);

	for (i = 0u; i < stage; ++i) {
		hal_diag.gpio[hal_gpio_gpset1] = hal_gpio42_mask;
		hal_rpiDiagDelay(hal_led_delay_loops);
		hal_diag.gpio[hal_gpio_gpclr1] = hal_gpio42_mask;
		hal_rpiDiagDelay(hal_led_delay_loops);
	}

	hal_rpiDiagDelay(hal_led_delay_loops * 4u);
}
#else
void hal_rpiDiagPulse(unsigned int stage)
{
	(void)stage;
}
#endif

/* parasoft-begin-suppress MISRAC2012-RULE_8_4 "Definition in assembly" */
syspage_t *hal_syspage;
u64 relOffs;
u32 schedulerLocked;
/* parasoft-end-suppress MISRAC2012-RULE_8_4 */


void *hal_syspageRelocate(void *data)
{
	return ((u8 *)data + relOffs);
}


ptr_t hal_syspageAddr(void)
{
	return (ptr_t)hal_syspage;
}


int hal_started(void)
{
	return hal_common.started;
}


void _hal_start(void)
{
	hal_common.started = 1;
}


void hal_lockScheduler(void)
{
#if NUM_CPUS != 1
	/* clang-format off */
	__asm__ volatile (
		"mov w1, #1\n"
		"b 2f\n"
	"1:\n"
		"wfe\n"
	"2:\n"
		"ldaxr w2, [%0]\n"
		"cbnz w2, 1b\n"
		"stxr w2, w1, [%0]\n"
		"cbnz w2, 2b\n"
	:
	: "r" (&schedulerLocked)
	: "x1", "x2", "memory");
	/* clang-format on */
#else
	/* Not necessary on single-core systems */
	(void)schedulerLocked;
	return;
#endif
}


__attribute__((section(".init"))) void _hal_init(void)
{
	const syspage_prog_t *dtb;
	addr_t dtbStart;
	addr_t dtbEnd;

	hal_rpiDiagPulse(10u);
	hal_common.started = 0;
	schedulerLocked = 0;
	_hal_spinlockInit();

	if ((hal_syspage->hs.firmwareDtb != 0u) && (hal_syspage->hs.firmwareDtbSize != 0u)) {
		dtbStart = hal_syspage->hs.firmwareDtb;
		dtbEnd = dtbStart + hal_syspage->hs.firmwareDtbSize;
		hal_consolePrint(ATTR_USER, "hal: using firmware dtb\n");
	}
	else {
		dtb = syspage_progNameResolve("system.dtb");
		if (dtb == NULL) {
#ifdef NDEBUG
			hal_cpuReboot();
#else
			for (;;) {
				hal_cpuHalt();
			}
#endif
		}

		dtbStart = dtb->start;
		dtbEnd = dtb->end;
		hal_consolePrint(ATTR_USER, "hal: using syspage dtb\n");
	}

	_pmap_preinit(dtbStart, dtbEnd);

	_hal_platformInit();
	_hal_consoleInit();
	hal_consolePrint(ATTR_USER, "hal: console init done\n");

	_hal_exceptionsInit();
	_hal_interruptsInit();

	_hal_cpuInit();

	_hal_timerInit(SYSTICK_INTERVAL);

	return;
}
