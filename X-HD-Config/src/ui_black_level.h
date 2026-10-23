#pragma once

#include "xbox_base.h"
#include "input.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLACK_LEVEL_RANGE_LIMITED = 0, // 16-235 (Standard Broadcast / HDMI Video)
    BLACK_LEVEL_RANGE_FULL    = 1  // 0-255  (Full Range PC RGB)
} black_level_range_t;

void BlackLevel_Init(void);
bool BlackLevel_IsActive(void);
void BlackLevel_SetActive(bool active);
void BlackLevel_HandleInput(const input_state_t *in);
void BlackLevel_Render(void);

#ifdef __cplusplus
}
#endif
