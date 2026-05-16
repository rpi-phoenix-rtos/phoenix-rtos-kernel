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


static inline void main_uartMark(char mark)
{
	volatile unsigned int *uart = (volatile unsigned int *)0xffffffffffe00000ull;
	volatile unsigned int *uartfr = (volatile unsigned int *)0xffffffffffe00018ull;

	while (*uartfr & 0x20) {}
	*uart = (unsigned int)mark;
}


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

	/* C-3 I-cache enable: enabling after the first scheduled context and
	 * _hal_start() reaches _usrv_start() but does not return. Move the
	 * boundary past user-server startup and test syspage/posix/spawn under
	 * I-cache.
	 */
	main_uartMark('D');
	hal_cpuDisableInterrupts();
	main_uartMark('i');
	hal_cpuEnableICache();
	main_uartMark('I');
	hal_consolePrint(ATTR_USER, "main_initthr: icache enabled\n");
	main_uartMark('a');

	main_uartMark('b');
	lib_printf("main: Starting syspage programs:");
	main_uartMark('c');
	syspage_progShow();
	main_uartMark('d');
	hal_consolePrint(ATTR_USER, "main_initthr: syspage listed\n");
	main_uartMark('e');

	main_uartMark('f');
	posix_init();
	main_uartMark('g');
	hal_consolePrint(ATTR_USER, "main_initthr: posix init done\n");
	main_uartMark('h');

	main_uartMark('E');
	hal_cpuEnableInterrupts();
	main_uartMark('j');
	(void)posix_clone(-1);
	main_uartMark('k');
	hal_consolePrint(ATTR_USER, "main_initthr: posix clone done\n");
	main_uartMark('l');

	/* Start programs from syspage */
	main_uartMark('m');
	prog = syspage_progList();
	main_uartMark('n');
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

	/* TD-16-1: timer + CPU-speed probe.
	 *
	 * Read CNTFRQ_EL0 and CNTPCT_EL0 deltas around a fixed nop-loop
	 * count to discriminate among:
	 *   (a) CNTFRQ matches actual hardware tick rate, CPU is fast
	 *       → both numbers near QEMU baseline.
	 *   (b) CNTFRQ matches but CPU runs slow (throttled / wrong PLL)
	 *       → cycles delta is near expected, but wall time stretches.
	 *   (c) CNTFRQ wrong → cycles delta out of proportion to nops.
	 *   (d) Both wrong → both numbers off.
	 *
	 * Output format: "td16:cntfrq=<8-hex> dt=<16-hex>\r\n"
	 * Loop body is 1<<20 (~1M) nops; a Cortex-A72 at 1.5 GHz with
	 * I-cache on takes ~700 us = ~38000 ticks at 54 MHz. Anything
	 * far above that points at CPU throttling / clock issue.
	 *
	 * Remove with the rest of TD-16 once the slowdown is understood.
	 */
	{
		static const char hexdig2[] = "0123456789abcdef";
		unsigned long cntfrq;
		unsigned long t0;
		unsigned long t1;
		unsigned long dt;
		unsigned long i;

		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
		__asm__ volatile("isb" ::: "memory");
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
		for (i = 0; i < (1UL << 20); ++i) {
			__asm__ volatile("nop");
		}
		__asm__ volatile("isb" ::: "memory");
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t1));
		dt = t1 - t0;

		while (*uartfr & 0x20) {}
		*uart = 't';
		while (*uartfr & 0x20) {}
		*uart = 'd';
		while (*uartfr & 0x20) {}
		*uart = '1';
		while (*uartfr & 0x20) {}
		*uart = '6';
		while (*uartfr & 0x20) {}
		*uart = ':';
		while (*uartfr & 0x20) {}
		*uart = 'c';
		while (*uartfr & 0x20) {}
		*uart = 'f';
		while (*uartfr & 0x20) {}
		*uart = '=';
		for (i = 8; i > 0; --i) {
			while (*uartfr & 0x20) {}
			*uart = hexdig2[(cntfrq >> ((i - 1) * 4)) & 0xfUL];
		}
		while (*uartfr & 0x20) {}
		*uart = ' ';
		while (*uartfr & 0x20) {}
		*uart = 'd';
		while (*uartfr & 0x20) {}
		*uart = 't';
		while (*uartfr & 0x20) {}
		*uart = '=';
		for (i = 16; i > 0; --i) {
			while (*uartfr & 0x20) {}
			*uart = hexdig2[(dt >> ((i - 1) * 4)) & 0xfUL];
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

	/* C-3v/w (2026-05-15): after the armstub applies conservative A72
	 * prefetch disables, retry the deferred D-cache enable path that
	 * previously hung on the first post-enable cacheable read. */
	while (*uartfr & 0x20) {}
	*uart = 'C'; /* C marker - about to enable D-cache */
	while (*uartfr & 0x20) {}
	*uart = 'd'; /* d marker - D-cache enable deferred */

	/* TD-16-1b: SECOND nop-loop measurement, AFTER _hal_init's
	 * cache-enable sequence. Compare to the first td16 result
	 * (pre-cache, in main() before _hal_init) — if dt drops by
	 * an order of magnitude or more, caches are doing their job.
	 * Same loop body so the comparison is direct.
	 */
	{
		static const char hexdig3[] = "0123456789abcdef";
		unsigned long cntfrq;
		unsigned long t0;
		unsigned long t1;
		unsigned long dt;
		unsigned long i;

		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
		__asm__ volatile("isb" ::: "memory");
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
		for (i = 0; i < (1UL << 20); ++i) {
			__asm__ volatile("nop");
		}
		__asm__ volatile("isb" ::: "memory");
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t1));
		dt = t1 - t0;

		while (*uartfr & 0x20) {}
		*uart = 't';
		while (*uartfr & 0x20) {}
		*uart = 'd';
		while (*uartfr & 0x20) {}
		*uart = '1';
		while (*uartfr & 0x20) {}
		*uart = '6';
		while (*uartfr & 0x20) {}
		*uart = 'b';
		while (*uartfr & 0x20) {}
		*uart = ':';
		while (*uartfr & 0x20) {}
		*uart = 'd';
		while (*uartfr & 0x20) {}
		*uart = 't';
		while (*uartfr & 0x20) {}
		*uart = '=';
		for (i = 16; i > 0; --i) {
			while (*uartfr & 0x20) {}
			*uart = hexdig3[(dt >> ((i - 1) * 4)) & 0xfUL];
		}
		while (*uartfr & 0x20) {}
		*uart = '\r';
		while (*uartfr & 0x20) {}
		*uart = '\n';
	}

	/* C-3 markers around the cache-enable transition. */
	while (*uartfr & 0x20) {}
	*uart = '1'; /* 1 marker - post-td16b */
	/* C-3x: direct UART marker here avoids the first post-D-cache
	 * hal_consolePrint path, which currently hangs before producing output
	 * on real Pi 4. Keep this narrow until the literal/function-entry hazard
	 * is localized. */
	while (*uartfr & 0x20) {}
	*uart = 'M';
	while (*uartfr & 0x20) {}
	*uart = '2'; /* 2 marker - post hal_consolePrint */

	/* Marker before usrv_init */
	while (*uartfr & 0x20) {}
	*uart = 'h'; /* h marker - before usrv_init */
	__asm__ volatile(
		"str xzr, [sp, #-16]!\n"
		"ldr xzr, [sp], #16\n"
		:
		:
		: "memory");
	while (*uartfr & 0x20) {}
	*uart = 'H'; /* H marker - post inline stack store */

	_usrv_init();

	while (*uartfr & 0x20) {}
	*uart = 'C'; /* C marker - cache enable still deferred */
	while (*uartfr & 0x20) {}
	*uart = 'c'; /* c marker - deferred-cache marker returned */

	/* Marker before version print */
	while (*uartfr & 0x20) {}
	*uart = 'v'; /* v marker - before version print */

	hal_consolePrint(ATTR_BOLD, "Phoenix-RTOS microkernel v. " RELEASE " rev. " VERSION "\n");
	hal_consolePrint(ATTR_USER, "J-after-banner\n");

	/* Marker after version print. Wait for FIFO space here: the previous
	 * no-wait marker could be dropped by PL011 when the banner filled FIFO,
	 * making the next boundary ambiguous under I-cache-on diagnostics.
	 */
	while (*uartfr & 0x20) {}
	*uart = 'j'; /* j marker - after version print */

	while (*uartfr & 0x20) {}
	*uart = 'k'; /* k marker - before cpu info print */
	lib_printf("hal: %s\n", hal_cpuInfo(s));
	while (*uartfr & 0x20) {}
	*uart = 'K'; /* K marker - after cpu info print */

	while (*uartfr & 0x20) {}
	*uart = 'l'; /* l marker - before cpu features print */
	lib_printf("hal: %s\n", hal_cpuFeatures(s, sizeof(s)));
	while (*uartfr & 0x20) {}
	*uart = 'L'; /* L marker - after cpu features print */

	while (*uartfr & 0x20) {}
	*uart = 'm'; /* m marker - before interrupts features print */
	lib_printf("hal: %s\n", hal_interruptsFeatures(s, sizeof(s)));
	while (*uartfr & 0x20) {}
	*uart = 'M'; /* M marker - after interrupts features print */

	while (*uartfr & 0x20) {}
	*uart = 'n'; /* n marker - before timer features print */
	lib_printf("hal: %s\n", hal_timerFeatures(s, sizeof(s)));
	while (*uartfr & 0x20) {}
	*uart = 'N'; /* N marker - after timer features print */

	while (*uartfr & 0x20) {}
	*uart = 'o'; /* o marker - before vm init */
	_vm_init(&main_common.kmap, &main_common.kernel);
	while (*uartfr & 0x20) {}
	*uart = 'O'; /* O marker - after vm init */

	hal_consolePrint(ATTR_USER, "main: vm init done\n");
	while (*uartfr & 0x20) {}
	*uart = 'p'; /* p marker - before perf init */
	(void)_perf_init(&main_common.kmap);
	while (*uartfr & 0x20) {}
	*uart = 'P'; /* P marker - after perf init */

	hal_consolePrint(ATTR_USER, "main: perf init done\n");
	while (*uartfr & 0x20) {}
	*uart = 'q'; /* q marker - before proc init */
	(void)_proc_init(&main_common.kmap, &main_common.kernel);
	while (*uartfr & 0x20) {}
	*uart = 'Q'; /* Q marker - after proc init */

	hal_consolePrint(ATTR_USER, "main: proc init done\n");
	while (*uartfr & 0x20) {}
	*uart = 'r'; /* r marker - before syscalls init */
	_syscalls_init();
	while (*uartfr & 0x20) {}
	*uart = 'R'; /* R marker - after syscalls init */

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

	while (*uartfr & 0x20) {}
	*uart = 's'; /* s marker - before init thread start */
	(void)proc_start(main_initthr, NULL, (const char *)"init");
	while (*uartfr & 0x20) {}
	*uart = 'S'; /* S marker - after init thread start */

	hal_consolePrint(ATTR_USER, "main: init thread started\n");

	/* Enter the first scheduled context before unmasking timer IRQs in this bootstrap context. */
	hal_consolePrint(ATTR_USER, "main: reschedule\n");
	(void)hal_cpuReschedule(NULL, NULL);

	return 0;
}
