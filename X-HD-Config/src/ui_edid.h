// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <stdbool.h>
#include <stdint.h>
#include "input.h"
#include "edid_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

// State management
void EDID_Init(void);
void EDID_RefreshFromBoard(void);
bool EDID_HasData(void);
const edid_parsed_t *EDID_GetParsed(void);

// Controller input handler for EDID tab (Mode toggle, Block scroll)
void EDID_HandleInput(const input_state_t *in);

// Render EDID tab (either Decoded Summary or Raw Hex Dump)
void EDID_Render(void);

#ifdef __cplusplus
}
#endif
