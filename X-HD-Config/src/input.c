#include "input.h"
#include <string.h>

#define MAX_PORTS               4
#define REPEAT_DELAY_FRAMES     15  // ~250 ms at 60 Hz
#define REPEAT_RATE_FRAMES      4   // ~66 ms at 60 Hz
#define ANALOG_THRESHOLD        48
#define THUMB_DEADZONE          16000

static HANDLE g_hPad[MAX_PORTS] = { NULL };
static WORD   g_wPrevButtons = 0;
static BYTE   g_bPrevAnalog[8] = { 0 };

static int    g_repeatTimerUp = 0;
static int    g_repeatTimerDown = 0;
static int    g_repeatTimerLeft = 0;
static int    g_repeatTimerRight = 0;

// BACK is a tap or a hold: a tap acts on release, a hold of INPUT_EXIT_HOLD_MS exits, so a
// hold never also runs BACK's tap action.
static DWORD  g_backDownTick = 0;
static bool   g_backHoldFired = false;

void input_init(void)
{
    XInitDevices(0, NULL);
}

static void update_devices(void)
{
    DWORD insertions = 0;
    DWORD removals = 0;

    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals) == TRUE) {
        for (DWORD i = 0; i < MAX_PORTS; i++) {
            if (removals & (1u << i)) {
                if (g_hPad[i]) {
                    XInputClose(g_hPad[i]);
                    g_hPad[i] = NULL;
                }
            }
            if (insertions & (1u << i)) {
                if (!g_hPad[i]) {
                    g_hPad[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
                }
            }
        }
    }

    // Fallback if initial device changes missed port 0
    if (!g_hPad[0]) {
        DWORD mask = XGetDevices(XDEVICE_TYPE_GAMEPAD);
        for (DWORD i = 0; i < MAX_PORTS; i++) {
            if ((mask & (1u << i)) && !g_hPad[i]) {
                g_hPad[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
            }
        }
    }
}

static bool check_repeat(bool held, int *timer)
{
    if (!held) {
        *timer = 0;
        return false;
    }
    if (*timer == 0) {
        *timer = 1;
        return true; // first press
    }
    (*timer)++;
    if (*timer >= REPEAT_DELAY_FRAMES) {
        if ((*timer - REPEAT_DELAY_FRAMES) % REPEAT_RATE_FRAMES == 0) {
            return true;
        }
    }
    return false;
}

void input_poll(input_state_t *out)
{
    memset(out, 0, sizeof(input_state_t));
    update_devices();

    // Poll first available connected pad
    HANDLE pad = NULL;
    for (int i = 0; i < MAX_PORTS; i++) {
        if (g_hPad[i]) {
            pad = g_hPad[i];
            break;
        }
    }

    if (!pad) {
        g_wPrevButtons = 0;
        memset(g_bPrevAnalog, 0, sizeof(g_bPrevAnalog));
        g_backDownTick = 0;
        g_backHoldFired = false;
        return;
    }

    XINPUT_STATE state;
    if (XInputGetState(pad, &state) != ERROR_SUCCESS) {
        return;
    }

    XINPUT_GAMEPAD *gp = &state.Gamepad;
    WORD currentDigital = gp->wButtons;
    WORD newlyPressed = (WORD)(currentDigital & ~g_wPrevButtons);

    out->raw_buttons = currentDigital;
    out->thumb_lx = gp->sThumbLX;
    out->thumb_ly = gp->sThumbLY;

    // Check stick directions
    bool stickUp = (gp->sThumbLY > THUMB_DEADZONE);
    bool stickDown = (gp->sThumbLY < -THUMB_DEADZONE);
    bool stickLeft = (gp->sThumbLX < -THUMB_DEADZONE);
    bool stickRight = (gp->sThumbLX > THUMB_DEADZONE);

    bool heldUp = ((currentDigital & XINPUT_GAMEPAD_DPAD_UP) != 0) || stickUp;
    bool heldDown = ((currentDigital & XINPUT_GAMEPAD_DPAD_DOWN) != 0) || stickDown;
    bool heldLeft = ((currentDigital & XINPUT_GAMEPAD_DPAD_LEFT) != 0) || stickLeft;
    bool heldRight = ((currentDigital & XINPUT_GAMEPAD_DPAD_RIGHT) != 0) || stickRight;

    out->up = check_repeat(heldUp, &g_repeatTimerUp);
    out->down = check_repeat(heldDown, &g_repeatTimerDown);
    out->left = check_repeat(heldLeft, &g_repeatTimerLeft);
    out->right = check_repeat(heldRight, &g_repeatTimerRight);

    // Analog action buttons (A, B, X, Y, BLACK, WHITE, L_TRIG, R_TRIG)
    // Indexes:
    // 0: A, 1: B, 2: X, 3: Y, 4: BLACK, 5: WHITE, 6: L_TRIG, 7: R_TRIG
    bool aDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_A] >= ANALOG_THRESHOLD);
    bool bDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_B] >= ANALOG_THRESHOLD);
    bool xDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_X] >= ANALOG_THRESHOLD);
    bool yDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_Y] >= ANALOG_THRESHOLD);
    bool blackDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_BLACK] >= ANALOG_THRESHOLD);
    bool whiteDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_WHITE] >= ANALOG_THRESHOLD);
    bool lTrigDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] >= ANALOG_THRESHOLD);
    bool rTrigDown = (gp->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] >= ANALOG_THRESHOLD);

    out->a = aDown && (g_bPrevAnalog[XINPUT_GAMEPAD_A] < ANALOG_THRESHOLD);
    out->b = bDown && (g_bPrevAnalog[XINPUT_GAMEPAD_B] < ANALOG_THRESHOLD);
    out->x = xDown && (g_bPrevAnalog[XINPUT_GAMEPAD_X] < ANALOG_THRESHOLD);
    out->y = yDown && (g_bPrevAnalog[XINPUT_GAMEPAD_Y] < ANALOG_THRESHOLD);
    out->black = blackDown && (g_bPrevAnalog[XINPUT_GAMEPAD_BLACK] < ANALOG_THRESHOLD);
    out->white = whiteDown && (g_bPrevAnalog[XINPUT_GAMEPAD_WHITE] < ANALOG_THRESHOLD);
    out->l_trigger = lTrigDown && (g_bPrevAnalog[XINPUT_GAMEPAD_LEFT_TRIGGER] < ANALOG_THRESHOLD);
    out->r_trigger = rTrigDown && (g_bPrevAnalog[XINPUT_GAMEPAD_RIGHT_TRIGGER] < ANALOG_THRESHOLD);

    out->start = (newlyPressed & XINPUT_GAMEPAD_START) != 0;

    // BACK: tap on release, exit on a long hold.
    bool backHeld = (currentDigital & XINPUT_GAMEPAD_BACK) != 0;
    if (backHeld) {
        DWORD now = GetTickCount();
        if (!(g_wPrevButtons & XINPUT_GAMEPAD_BACK)) {
            g_backDownTick = now;
            g_backHoldFired = false;
        }
        out->back_hold_ms = now - g_backDownTick;
        if (!g_backHoldFired && out->back_hold_ms >= INPUT_EXIT_HOLD_MS) {
            g_backHoldFired = true;
            out->exit_requested = true;
        }
    } else if (g_wPrevButtons & XINPUT_GAMEPAD_BACK) {
        out->back = !g_backHoldFired;
        g_backHoldFired = false;
    }

    g_wPrevButtons = currentDigital;
    memcpy(g_bPrevAnalog, gp->bAnalogButtons, sizeof(g_bPrevAnalog));
}

bool input_has_action(const input_state_t *in)
{
    if (!in) return false;
    if (in->exit_requested) return true;
    if (in->up || in->down || in->left || in->right) return true;
    if (in->a || in->b || in->x || in->y) return true;
    if (in->white || in->black || in->start || in->back) return true;
    if (in->l_trigger || in->r_trigger) return true;
    if (in->thumb_ly > THUMB_DEADZONE || in->thumb_ly < -THUMB_DEADZONE) return true;
    if (in->thumb_lx > THUMB_DEADZONE || in->thumb_lx < -THUMB_DEADZONE) return true;
    return false;
}

