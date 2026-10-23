#pragma once

#include "xbox_base.h"
#include "input.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void About_Init(void);
void About_HandleInput(const input_state_t *in);
void About_Render(void);

#ifdef __cplusplus
}
#endif
