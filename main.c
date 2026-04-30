/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Kernel initialization
 *
 * Copyright 2012-2017, 2021 Phoenix Systems
 * Copyright 2001, 2005-2006 Pawel Pisarczyk
 * Author: Pawel Pisarczyk, Aleksander Kaminski, Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "hal/hal.h"

#include "usrv.h"
#include "lib/lib.h"
#include "vm/vm.h"
#include "proc/proc.h"
#include "posix/posix.h"
#include "syscalls.h"
#include "syspage.h"
#include "test/test.h"
#include "perf/perf.h"


static struct {
	vm_map_t kmap;
	vm_object_t kernel;
	page_t *page;
	void *stack;
	size_t stacksz;
} main_common;


static void main_initthr(void *unused)
{
	int res;
	unsigned int argc;

	syspage_prog_t *prog;
	char *argv[32], *cmdline;
	char msg[96];

	hal_consolePrint(ATTR_USER, "main_initthr: enter\n");

	/* Enable locking and multithreading related mechanisms */
	_hal_start();
	hal_consolePrint(ATTR_USER, "main_initthr: hal started\n");

	_usrv_start();
	hal_consolePrint(ATTR_USER, "main_initthr: usrv started\n");

	lib_printf("main: Starting syspage programs:");
	syspage_progShow();
	hal_consolePrint(ATTR_USER, "main_initthr: syspage listed\n");

	posix_init();
	hal_consolePrint(ATTR_USER, "main_initthr: posix init done\n");

	(void)posix_clone(-1);
	hal_consolePrint(ATTR_USER, "main_initthr: posix clone done\n");

	/* Start programs from syspage */
	prog = syspage_progList();
	if (prog != NULL) {
		do {
			cmdline = prog->argv;
			/* If app shouldn't be executed then args should be discarded */
			if (*cmdline != 'X') {
				while (*cmdline != ';' && *cmdline != '\0') {
					++cmdline;
				}

				*cmdline = '\0';
				continue;
			}

			/* 'X' is no longer useful */
			++prog->argv;
			cmdline = prog->argv;
			argc = 0;
			while (argc < (sizeof(argv) / sizeof(*argv) - 1U)) {
				argv[argc] = cmdline;
				argc++;
				while (*cmdline != ';' && *cmdline != '\0') {
					++cmdline;
				}

				if (*cmdline == '\0') {
					break;
				}

				*(cmdline++) = '\0';
			}
			argv[argc] = NULL;

			(void)lib_sprintf(msg, "main: spawn %s\n", argv[0]);
			hal_consolePrint(ATTR_USER, msg);
			res = proc_syspageSpawn(prog, vm_getSharedMap((int)prog->imaps[0]), vm_getSharedMap((int)prog->dmaps[0]), argv[0], argv);
			if (res < 0) {
				(void)lib_sprintf(msg, "main: failed to spawn %s (%d)\n", argv[0], res);
				hal_consolePrint(ATTR_USER, msg);
			}
			else {
				(void)lib_sprintf(msg, "main: spawned %s (%d)\n", argv[0], res);
				hal_consolePrint(ATTR_USER, msg);
			}
		} while ((prog = prog->next) != syspage_progList());
	}

	for (;;) {
		proc_reap();
	}
}


int main(void)
{
	char s[128];
	
	/* DEBUG: Send marker immediately upon entry.
	 * After MMU enable, use the TTBR1-mapped early PL011 alias. */
	volatile unsigned int *uart = (volatile unsigned int *)0xffffffffffe00000ull;
	volatile unsigned int *uartfr = (volatile unsigned int *)0xffffffffffe00018ull;
	
	/* Wait for UART to be ready */
	while (*uartfr & 0x20) {}
	
	/* Send 'c' marker - we reached main()! */
	*uart = 'c';
	
	/* Send additional markers to confirm execution */
	while (*uartfr & 0x20) {}
	*uart = 'd'; /* d marker - main() executing */

	/* Marker before syspage_init */
	while (*uartfr & 0x20) {}
	*uart = 'e'; /* e marker - before syspage_init */

	syspage_init();

	/* Marker before hal_init */
	while (*uartfr & 0x20) {}
	*uart = 'f'; /* f marker - before hal_init */

	_hal_init();

	/* Marker after hal_init, before console print */
	while (*uartfr & 0x20) {}
	*uart = 'g'; /* g marker - after hal_init */

	hal_consolePrint(ATTR_USER, "main: hal init done\n");

	/* Marker before usrv_init */
	while (*uartfr & 0x20) {}
	*uart = 'h'; /* h marker - before usrv_init */

	_usrv_init();

	/* Marker before version print */
	while (*uartfr & 0x20) {}
	*uart = 'i'; /* i marker - before version print */

	hal_consolePrint(ATTR_BOLD, "Phoenix-RTOS microkernel v. " RELEASE " rev. " VERSION "\n");

	/* Marker after version print */
	while (*uartfr & 0x20) {}
	*uart = 'j'; /* j marker - after version print */

	lib_printf("hal: %s\n", hal_cpuInfo(s));
	lib_printf("hal: %s\n", hal_cpuFeatures(s, sizeof(s)));
	lib_printf("hal: %s\n", hal_interruptsFeatures(s, sizeof(s)));
	lib_printf("hal: %s\n", hal_timerFeatures(s, sizeof(s)));

	_vm_init(&main_common.kmap, &main_common.kernel);
	hal_consolePrint(ATTR_USER, "main: vm init done\n");
	(void)_perf_init(&main_common.kmap);
	hal_consolePrint(ATTR_USER, "main: perf init done\n");
	(void)_proc_init(&main_common.kmap, &main_common.kernel);
	hal_consolePrint(ATTR_USER, "main: proc init done\n");
	_syscalls_init();
	hal_consolePrint(ATTR_USER, "main: syscalls init done\n");

#if 0 /* Basic kernel tests */
	/* Start tests */
	test_proc_threads1();
	test_vm_kmallocsim();
	test_proc_conditional();
	test_vm_alloc();
	test_vm_kmalloc();
	test_proc_exit();
#endif

	(void)proc_start(main_initthr, NULL, (const char *)"init");
	hal_consolePrint(ATTR_USER, "main: init thread started\n");

	/* Enter the first scheduled context before unmasking timer IRQs in this bootstrap context. */
	hal_consolePrint(ATTR_USER, "main: reschedule\n");
	(void)hal_cpuReschedule(NULL, NULL);

	return 0;
}
