#pragma once

#define DEBUG_OUT

#include <stdint.h>
#include <stdbool.h>
#include "types.h"
#include "defines.h"

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
