#pragma once

#include "xbox_base.h"
#include <stdbool.h>
#include <stdint.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

// Open the On-Screen Keyboard editor with the current 24-byte SPD buffer
void OSK_Open(const char currentSpd[25]);

// Check if the OSK editor modal is currently active
bool OSK_IsActive(void);

// Process controller input when OSK is active
// Returns true if the editor closed this frame (committed or cancelled)
bool OSK_HandleInput(const input_state_t *in, bool *outCommitted, char outSpd[25]);

// Render the 720p retro green-phosphor OSK interface
void OSK_Render(void);

#ifdef __cplusplus
}
#endif
