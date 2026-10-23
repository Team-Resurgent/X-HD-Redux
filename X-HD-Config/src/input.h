#pragma once

#include "xbox_base.h"
#include <stdbool.h>
#include <stdint.h>
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_EXIT_HOLD_MS 1500

typedef struct {
    // Buttons pressed this frame (single trigger)
    bool up;
    bool down;
    bool left;
    bool right;
    bool a;
    bool b;
    bool x;
    bool y;
    bool white;
    bool black;
    bool start;
    bool back;
    bool l_trigger;
    bool r_trigger;

    // Analog stick deltas
    int16_t thumb_lx;
    int16_t thumb_ly;

    // Raw digital buttons held
    WORD raw_buttons;

    // BACK held for INPUT_EXIT_HOLD_MS: exit to the dashboard. back_hold_ms is how long BACK
    // has been down so far (0 when it isn't), for an on-screen hint while it's held.
    bool exit_requested;
    DWORD back_hold_ms;
} input_state_t;

// Initialize Xbox controller subsystem
void input_init(void);

// Poll controller state once per frame
void input_poll(input_state_t *outInput);

// Returns true if any actionable user input occurred
bool input_has_action(const input_state_t *in);

#ifdef __cplusplus
}
#endif
