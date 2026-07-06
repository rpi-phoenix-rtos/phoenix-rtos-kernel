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
#if defined(__TARGET_AARCH64A72) || defined(__TARGET_AARCH64A53)
#include "hal/aarch64/aarch64.h"
#endif

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

	/* Enable locking and multithreading related mechanisms */
	_hal_start();
	_usrv_start();

	/* I-cache is already enabled at MMU bring-up time in
	 * hal/aarch64/_init.S via the single-shot SCTLR_EL1.{M,C,I} write,
	 * so the historical `hal_cpuEnableICache()` deferred-enable call
	 * here is redundant. Removed 2026-05-17 with the cache-era debug
	 * cleanup. */

	lib_printf("main: Starting syspage programs:");
	syspage_progShow();

	posix_init();

	(void)posix_clone(-1);

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

			res = proc_syspageSpawn(prog, vm_getSharedMap((int)prog->imaps[0]), vm_getSharedMap((int)prog->dmaps[0]), argv[0], argv);
			if (res < 0) {
				lib_printf("main: failed to spawn %s (%d)\n", argv[0], res);
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

	syspage_init();
	_hal_init();
	_usrv_init();

	hal_consolePrint(ATTR_BOLD, "Phoenix-RTOS microkernel v. " RELEASE " rev. " VERSION "\n");

	lib_printf("hal: %s\n", hal_cpuInfo(s));
	lib_printf("hal: %s\n", hal_cpuFeatures(s, sizeof(s)));
	lib_printf("hal: %s\n", hal_interruptsFeatures(s, sizeof(s)));
	lib_printf("hal: %s\n", hal_timerFeatures(s, sizeof(s)));

	_vm_init(&main_common.kmap, &main_common.kernel);
	hal_consolePrint(ATTR_USER, "hi: vm-done\n");
	(void)_perf_init(&main_common.kmap);
	hal_consolePrint(ATTR_USER, "hi: perf-done\n");
	(void)_proc_init(&main_common.kmap, &main_common.kernel);
	hal_consolePrint(ATTR_USER, "hi: proc-done\n");
	_syscalls_init();
	hal_consolePrint(ATTR_USER, "hi: syscalls-done\n");

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
	hal_consolePrint(ATTR_USER, "hi: proc-start-done\n");

	/* SMP-D-5: publish primary-ready flag. Secondaries spin-wait on
	 * this in _other_core_virtual before invoking the per-CPU C
	 * init helpers, so they don't race primary's vm/proc/threads
	 * setup. With main_initthr now spawned and the scheduler about
	 * to take over, vm/proc/threads are stable enough for
	 * secondaries to bring themselves up safely. */
	/* hal_smpPrimaryReady / hal_smpFirstIntervalUs are aarch64-only SMP
	 * bring-up state; gate on __aarch64__ so this shared main.c still links
	 * on other SMP targets (sparcv8leon gr740/gr712rc), which have their own
	 * secondary-CPU start path and don't provide these symbols. */
#if (NUM_CPUS != 1) && defined(__aarch64__)
	{
		extern volatile unsigned int hal_smpPrimaryReady;
		extern volatile unsigned int hal_smpFirstIntervalUs;

		/* SMP D-8: defer the FIRST secondary timer PPI by 10 s so
		 * primary's hal_cpuReschedule + main_initthr boot completes
		 * before any secondary IRQ fires. Once main_initthr is
		 * established, secondaries fall into the normal SYSTICK
		 * cadence via threads_timeintr's re-arm path. */
		hal_smpFirstIntervalUs = 10000000U;

		hal_smpPrimaryReady = 1U;
		hal_cpuDataSyncBarrier();
		hal_cpuSignalEvent();
	}
#endif

	/* Enter the first scheduled context before unmasking timer IRQs in this bootstrap context. */
	(void)hal_cpuReschedule(NULL, NULL);
	hal_consolePrint(ATTR_USER, "hi: reschedule-done\n");

	return 0;
}
