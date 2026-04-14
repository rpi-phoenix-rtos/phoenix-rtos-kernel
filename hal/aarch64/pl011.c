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

void hal_pl011Putch(hal_pl011_t *uart, char c)
{
	while ((*(uart->base + pl011_fr) & PL011_FR_TXFF) != 0U) {
	}

	*(uart->base + pl011_dr) = (u32)(u8)c;
}


void hal_pl011Flush(hal_pl011_t *uart)
{
	while ((*(uart->base + pl011_fr) & PL011_FR_BUSY) != 0U) {
	}
}
