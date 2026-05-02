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
			if (++spawnIters >= 32) {
				hal_consolePrint(ATTR_USER, "main: TD-13 spawn-cap hit, breaking spawn loop\n");
				break;
			}
		} while ((prog = prog->next) != syspage_progList());
	}

	/* TD-13 probe: confirm main() reaches the proc_reap idle loop.
	 * If this prints but user processes still produce no UART output,
	 * the silence is downstream (pl011-tty / /dev/console binding).
	 * If this DOES NOT print, something is hanging between the spawn
	 * loop's last iteration and entering proc_reap. */
	hal_consolePrint(ATTR_USER, "main: spawn loop done, entering proc_reap idle\n");

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

	/* TD-15 phase 1: VC4 mailbox-buffer drift probe.
	 *
	 * Plo writes a known 64-byte pattern at PA PLO_RPI_MAILBOX_BUFFER_ADDRESS
	 * just before eret (see plo/hal/aarch64/generic/hal.c
	 * hal_td15ProbeWrite). The kernel maps that PA at VA 0xffffffffffe01000
	 * Normal Non-Cacheable in _init.S. Compare what we read here to the
	 * expected pattern. Drift = some agent (suspected VC4) is writing to
	 * ARM-usable DRAM after plo's eret. Result tagged "td15:" for grep.
	 * Remove with the rest of TD-15 phase 1 once VC6 hygiene is closed.
	 */
	{
		volatile const unsigned int *mboxAlias = (volatile const unsigned int *)0xffffffffffe01000ull;
		unsigned int seed = 0xa5a5a5a5u;
		unsigned int diffs = 0u;
		unsigned int firstBad = 16u;
		unsigned int gotFirst = 0u;
		unsigned int i;
		for (i = 0u; i < 16u; ++i) {
			unsigned int expected = seed ^ (i * 0x01010101u);
			unsigned int got = mboxAlias[i];
			if (got != expected) {
				if (gotFirst == 0u) {
					firstBad = i;
					gotFirst = got;
				}
				diffs++;
			}
		}
		while (*uartfr & 0x20) {}
		*uart = 't';
		while (*uartfr & 0x20) {}
		*uart = 'd';
		while (*uartfr & 0x20) {}
		*uart = '1';
		while (*uartfr & 0x20) {}
		*uart = '5';
		while (*uartfr & 0x20) {}
		*uart = ':';
		if (diffs == 0u) {
			while (*uartfr & 0x20) {}
			*uart = 'O';
			while (*uartfr & 0x20) {}
			*uart = 'K';
		}
		else {
			while (*uartfr & 0x20) {}
			*uart = 'D';
			while (*uartfr & 0x20) {}
			*uart = '=';
			/* dump diff count + first-bad index as ASCII hex digits */
			static const char hexdig[] = "0123456789abcdef";
			while (*uartfr & 0x20) {}
			*uart = hexdig[(diffs >> 4) & 0xfu];
			while (*uartfr & 0x20) {}
			*uart = hexdig[diffs & 0xfu];
			while (*uartfr & 0x20) {}
			*uart = '/';
			while (*uartfr & 0x20) {}
			*uart = hexdig[(firstBad >> 4) & 0xfu];
			while (*uartfr & 0x20) {}
			*uart = hexdig[firstBad & 0xfu];
		}
		while (*uartfr & 0x20) {}
		*uart = '\r';
		while (*uartfr & 0x20) {}
		*uart = '\n';
	}

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
