// SPDX-License-Identifier: GPL-3.0-only
// What main.c offers the other screens.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Power-cycle the console through the SMC. With settings changed since the last save it first
// asks: save then reboot, reboot without saving, or stay.
void App_RequestReboot(void);

#ifdef __cplusplus
}
#endif
