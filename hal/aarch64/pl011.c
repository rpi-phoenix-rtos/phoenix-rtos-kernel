/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * PL011 helper
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "pl011.h"

#include "hal/cpu.h"

#include "arch/pmap.h"


enum {
        pl011_dr = 0x00 / sizeof(u32),
        pl011_fr = 0x18 / sizeof(u32),
        pl011_ibrd = 0x24 / sizeof(u32),
        pl011_fbrd = 0x28 / sizeof(u32),
        pl011_lcrh = 0x2c / sizeof(u32),
        pl011_cr = 0x30 / sizeof(u32),
};


#define PL011_FR_BUSY (1U << 3)
#define PL011_FR_TXFF (1U << 5)

#define PL011_FIFO_SIZE 32U

/* Status reads to wait for one character to leave a full TX FIFO. At 115200
 * baud that takes ~87 us; this bound is milliseconds on any core, so only a
 * UART that has stopped transmitting reaches it. */
#define PL011_TX_SPINS 200000U


int hal_pl011Init(hal_pl011_t *uart, addr_t base)
{
        if ((uart == NULL) || (base == 0U)) {
                return -1;
        }

        uart->base = _pmap_halMapDevice(base, 0, SIZE_PAGE);
        if (uart->base == NULL) {
                return -1;
        }

        /* Hardcode 115200 baud rate (assuming 48MHz clock)
         * IBRD = 26, FBRD = 3
         */
        *(uart->base + pl011_cr) = 0; /* Disable UART */
        *(uart->base + pl011_ibrd) = 26;
        *(uart->base + pl011_fbrd) = 3;
        *(uart->base + pl011_lcrh) = 0x70; /* 8N1, FIFO enabled */
        *(uart->base + pl011_cr) = 0x301; /* Enable UART, TX, RX */

        return 0;
}

int hal_pl011Putch(hal_pl011_t *uart, char c)
{
	/* A stalled UART gets one look: do not spend the full wait per character */
	u32 spins = (uart->stalled != 0U) ? 1U : PL011_TX_SPINS;

	while ((*(uart->base + pl011_fr) & PL011_FR_TXFF) != 0U) {
		spins--;
		if (spins == 0U) {
			uart->stalled = 1U;
			hal_cpuAtomicInc(&uart->dropped);
			return -1;
		}
	}

	uart->stalled = 0U;
	*(uart->base + pl011_dr) = (u32)(u8)c;

	return 0;
}


void hal_pl011Flush(hal_pl011_t *uart)
{
	u32 spins = PL011_FIFO_SIZE * PL011_TX_SPINS;

	while (((*(uart->base + pl011_fr) & PL011_FR_BUSY) != 0U) && (spins != 0U)) {
		spins--;
	}
}
