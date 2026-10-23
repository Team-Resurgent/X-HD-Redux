#include "error_handler.h"
#include "debug.h"

void _Error_Handler(int line) {
    debug_log("Error_Handler line %d\r\n", line);
    while (1) {}
}
