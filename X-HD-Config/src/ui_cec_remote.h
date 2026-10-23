#pragma once

#include "xbox_base.h"
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

void CECRemote_Init(void);
bool CECRemote_Update(bool isActive);
void CECRemote_HandleInput(const input_state_t *in);
void CECRemote_Render(void);

#ifdef __cplusplus
}
#endif
