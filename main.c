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
		/* TODO(TD-13-spawn-cap): hard cap on spawn-loop iterations on
		 * real Pi 4. Empirically the loop iterates correctly through
		 * all 9 progs (dummyfs-root → … → psh) once, prints each
		 * legitimate "spawned X (PID)" line, and then keeps printing
		 * "main: spawned psh (9)" tens of thousands of times — i.e.
		 * the do-while termination
		 *     `(prog = prog->next) != syspage_progList()`
		 * never fires because `psh->next` doesn't return to the head
		 * on real hardware (works in QEMU). Same shape as TD-04
		 * cache-coherency on the syspage. Cap forces a clean exit so
		 * the rest of the kernel (and the spawned user processes) can
		 * actually run.
		 *
		 * Bound is 32 (well above the 9 progs we ship, well below the
		 * 187 K iters seen without the cap). Once TD-13 root cause is
		 * understood, this cap becomes a redundancy. */
		int spawnIters = 0;
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
			if (++spawnIters >= 32) {
				lib_printf("main: TD-13 spawn-cap hit, breaking spawn loop\n");
				break;
			}
		} while ((prog = prog->next) != syspage_progList());
	}

#if NUM_CPUS != 1
	/* SMP Phase D observability: print per-CPU bring-up + tick counters
	 * so we can pinpoint where the secondary path stalls.
	 *   intr:     count of _hal_interruptsInitPerCPU entries
	 *   ppi:      count of timer-PPI enable in GICD_ISENABLER0 (secondaries only)
	 *   tmr:      count of _hal_timerInitPerCPU entries (secondaries only)
	 *   tick:     count of threads_timeintr entries (timer ISR runs)
	 *
	 * If intr>0 ppi>0 tmr>0 but tick=0 for cpu N → IRQs not delivered
	 * to that core (DAIF, GIC routing, or vbar issue).
	 * If intr>0 ppi>0 tmr=0 → secondary crashed between PPI enable and
	 * timer arm (_hal_cpuInit path).
	 * If intr=0 → secondary never reached _hal_interruptsInitPerCPU
	 * (stack bug or _other_core_virtual path broken).
	 *
	 * Use hal_consolePrint not lib_printf: by the time main_initthr
	 * gets here the log subsystem has been enabled, so lib_printf
	 * output is buffered to klog (which has no /dev/klog drain on
	 * the Pi 4 boot path). hal_consolePrint writes UART directly. */
	{
		extern volatile unsigned int threads_smpTickCount[8];
		extern volatile unsigned int hal_smpInterruptsInitPerCpuCount[8];
		extern volatile unsigned int hal_smpInterruptsEnabledPpiCount[8];
		extern volatile unsigned int hal_smpTimerInitPerCpuCount[8];
		unsigned int i;
		char buf[160];
		char *p;
		unsigned int n = (unsigned int)hal_cpuGetCount();
		if (n > 8U) {
			n = 8U;
		}

		p = buf;
		p += lib_sprintf(p, "smp: intr");
		for (i = 0; i < n; i++) {
			p += lib_sprintf(p, " cpu%u=%u", i, hal_smpInterruptsInitPerCpuCount[i]);
		}
		p += lib_sprintf(p, "\n");
		hal_consolePrint(ATTR_USER, buf);

		p = buf;
		p += lib_sprintf(p, "smp: ppi ");
		for (i = 0; i < n; i++) {
			p += lib_sprintf(p, " cpu%u=%u", i, hal_smpInterruptsEnabledPpiCount[i]);
		}
		p += lib_sprintf(p, "\n");
		hal_consolePrint(ATTR_USER, buf);

		p = buf;
		p += lib_sprintf(p, "smp: tmr ");
		for (i = 0; i < n; i++) {
			p += lib_sprintf(p, " cpu%u=%u", i, hal_smpTimerInitPerCpuCount[i]);
		}
		p += lib_sprintf(p, "\n");
		hal_consolePrint(ATTR_USER, buf);

		p = buf;
		p += lib_sprintf(p, "smp: tick");
		for (i = 0; i < n; i++) {
			p += lib_sprintf(p, " cpu%u=%u", i, threads_smpTickCount[i]);
		}
		p += lib_sprintf(p, "\n");
		hal_consolePrint(ATTR_USER, buf);

	}
#endif

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
#if NUM_CPUS != 1
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
	hal_consolePrint(ATTR_USER, "hi: primary-ready set\n");
#endif

	/* Enter the first scheduled context before unmasking timer IRQs in this bootstrap context. */
	(void)hal_cpuReschedule(NULL, NULL);
	hal_consolePrint(ATTR_USER, "hi: reschedule-done\n");

	return 0;
}
