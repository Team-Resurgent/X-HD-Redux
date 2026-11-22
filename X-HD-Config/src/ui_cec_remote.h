// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

void CECRemote_Init(void);
// Every frame. Reads the X-HD's CEC relay (at most every 100 ms) unless may_poll is false (a
// firmware job owns the bus). TV remote keys drive the app as D-pad / (A) / (B) taps --
// collected for CECRemote_ApplyNav() -- except during the tester tab's test, where they only
// light up the on-screen remote. Leaving the tab (on_tab false) ends the test. True when the
// screen needs redrawing.
bool CECRemote_Update(bool on_tab, bool may_poll);
// ORs the remote's pending taps into this frame's input, then clears them. With lr_tabs the
// remote's Left/Right switch tabs (White/Black); without, they are D-pad Left/Right.
void CECRemote_ApplyNav(input_state_t *in, bool lr_tabs);
// The tester tab's test is on: (A) starts it, (B) or a held remote Back ends it.
bool CECRemote_IsTesting(void);
// CEC frames read from the board since the app started (every tab polls them).
void CECRemote_HandleInput(const input_state_t *in);
void CECRemote_Render(void);

#ifdef __cplusplus
}
#endif
