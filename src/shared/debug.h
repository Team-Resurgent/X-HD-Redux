// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#define DEBUG_OUT

#include <stdint.h>
#include <stdbool.h>
#include "types.h"
#include "defines.h"
#include "log_codes.h"   // debug_log(LOG_...): coded lines, expanded host-side

#ifdef DEBUG_OUT

void debug_init(void);
void debug_log(const char *fmt, ...);
void debug_ring_log(const char *fmt, ...);
void debug_puts(const char *str);
void debug_ring_flush(void);
void debug_cli_poll(const xbox_encoder xb_encoder);
void debug_print_full_dump(void);

// Pop one byte from the USART2 RX ring (filled by USART2_IRQHandler), or return
// -1 if the ring is empty. Used by the bootloader UART firmware updater; the
// application drains the same ring via debug_cli_poll().
#ifndef BUILD_APPLICATION
int debug_uart_getc(void);
#else
// Waits until everything queued for the UART (sent by DMA) is out: before a reset.
void debug_tx_drain(void);
#endif

#else

#define debug_init()                     ((void)0)
#define debug_log(...)                   ((void)0)
#define debug_puts(str)                  ((void)0)
#define debug_ring_log(...)              ((void)0)
#define debug_ring_flush()               ((void)0)
#define debug_cli_poll(xb_encoder)       ((void)0)
#define debug_print_full_dump()          ((void)0)
#define debug_uart_getc()                (-1)

#endif
