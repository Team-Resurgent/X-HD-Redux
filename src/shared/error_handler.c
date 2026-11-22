// SPDX-License-Identifier: GPL-3.0-only
#include "error_handler.h"
#include "debug.h"

void _Error_Handler(int line) {
    debug_log("Error_Handler line %d\n", line);
    while (1) {}
}
