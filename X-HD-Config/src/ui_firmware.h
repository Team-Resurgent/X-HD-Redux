// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include "input.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void Firmware_Init(void);
// The tab was opened: find the board and the images (unless a job is running).
void Firmware_OnEnter(void);
// A job is running: the tab owns the screen and the bus until it ends.
bool Firmware_IsBusy(void);
// Once per frame. True when the screen needs redrawing. *board_changed is set when a job
// ended that may have changed the board's mode or firmware.
bool Firmware_Update(bool *board_changed);
void Firmware_HandleInput(const input_state_t *in);
// The image list's choice: select it and compare it with the board.
void Firmware_PickImage(int index);
void Firmware_Render(void);
const char *Firmware_Footer(void);
// An update or conversion finished: only the console reboot is left, everywhere in the app.
bool Firmware_RebootPending(void);
// Startup: no X-HD application answers, so this tab is the useful one.
bool Firmware_WantsFocus(void);

#ifdef __cplusplus
}
#endif
