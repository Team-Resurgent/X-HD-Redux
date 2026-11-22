// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <stdbool.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

// A boxed choice list drawn over the current tab in the terminal's colours: Up/Down move,
// (A) chooses, (B) or (BACK) cancels. The item strings must outlive the open list.
#define PICK_MAX_ITEMS 24

typedef enum { PICK_NONE, PICK_CHOSEN, PICK_CANCELLED } pick_result_t;

// Which setting a list answers for (main.c routes the choice).
typedef enum {
    PICK_ID_COLOR_SPACE, PICK_ID_ASPECT, PICK_ID_SCAN, PICK_ID_CLOCK_PHASE, PICK_ID_CEC_STANDBY,
    PICK_ID_SPD, PICK_ID_VIDEO_MODE, PICK_ID_FW_IMAGE, PICK_ID_PIC_PROFILE,
    PICK_ID_LEAVE           // unsaved changes: save, discard or stay before leaving
} pick_id_t;

// id tells the caller which list answered. The box's title sits on `row`, `indent_px` right
// of the left margin; it moves up if it would run into the footer.
void Picklist_Open(int id, const char *title, const char *const *items, int count, int current,
                   int row, int indent_px);
bool Picklist_IsActive(void);
int  Picklist_Id(void);
// PICK_CHOSEN sets *chosen; the list closes on PICK_CHOSEN and PICK_CANCELLED.
pick_result_t Picklist_HandleInput(const input_state_t *in, int *chosen);
void Picklist_Render(void);

#ifdef __cplusplus
}
#endif
