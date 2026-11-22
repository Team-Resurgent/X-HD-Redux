// SPDX-License-Identifier: GPL-3.0-only
#include "ui_cec_remote.h"
#include "ui_terminal.h"
#include "smbus_hal.h"
#include <stdio.h>
#include <string.h>

#define MAX_CEC_LOG_ENTRIES 7

typedef struct {
    char text[64];
    DWORD color;
} cec_log_entry_t;

static uint8_t  s_highlight_timer[256];
static uint8_t  s_last_key_code = 0xFF;
static char     s_last_key_name[32] = "None";
static char     s_last_action_name[48] = "Waiting for TV remote...";
static char     s_last_raw_hex[48] = "None";
static uint32_t s_total_frames = 0;
static bool     s_key_is_pressed = false;
static uint8_t  s_held_key = 0xFF;

static cec_log_entry_t s_log[MAX_CEC_LOG_ENTRIES];
static int             s_log_count = 0;

// TV remote -> app navigation. Unlabelled on purpose (a surprise for whoever tries it). CEC
// User Control codes: Select 0x00, Up 0x01, Down 0x02, Left 0x03, Right 0x04, Exit 0x0D (what
// most remotes' Back/Return key sends). Each <User Control Pressed> is one tap; a TV repeats it
// while a key is held, which repeats the tap like a held D-pad. Exit is a (B) tap, never the
// hold-BACK exit.
static input_state_t s_nav;

// The tester tab's test: while on, every key only lights up the on-screen remote, and holding
// the remote's Back for TEST_STOP_HOLD_MS (press to release, or to the latest repeat) ends it.
#define TEST_STOP_HOLD_MS 1000
static bool  s_testing = false;
static DWORD s_back_since = 0;

static void nav_key(uint8_t key)
{
    switch (key) {
        case 0x00: s_nav.a = true; break;
        case 0x01: s_nav.up = true; break;
        case 0x02: s_nav.down = true; break;
        case 0x03: s_nav.left = true; break;
        case 0x04: s_nav.right = true; break;
        case 0x0D: s_nav.b = true; break;
        case 0x91: s_nav.b = true; break;   // Samsung's Return
        default: break;
    }
}

void CECRemote_ApplyNav(input_state_t *in, bool lr_tabs)
{
    in->up |= s_nav.up;
    in->down |= s_nav.down;
    if (lr_tabs) {
        in->white |= s_nav.left;
        in->black |= s_nav.right;
    } else {
        in->left |= s_nav.left;
        in->right |= s_nav.right;
    }
    in->a |= s_nav.a;
    in->b |= s_nav.b;
    memset(&s_nav, 0, sizeof(s_nav));
}

// CEC <User Control Pressed> key codes (CEC 1.4 table 27; the names follow libCEC's cectypes.h).
// Samsung sends some keys as <Vendor Remote Button Down> with the same codes, plus 0x91 Return.
static const char *get_key_name(uint8_t key)
{
    static const char *const kNames[0x92] = {
        [0x00] = "SELECT / OK", [0x01] = "UP", [0x02] = "DOWN", [0x03] = "LEFT", [0x04] = "RIGHT",
        [0x05] = "RIGHT-UP", [0x06] = "RIGHT-DOWN", [0x07] = "LEFT-UP", [0x08] = "LEFT-DOWN",
        [0x09] = "ROOT MENU", [0x0A] = "SETUP MENU", [0x0B] = "CONTENTS MENU", [0x0C] = "FAVORITE MENU",
        [0x0D] = "BACK / EXIT", [0x10] = "TOP MENU", [0x11] = "DVD MENU", [0x1D] = "NUMBER ENTRY",
        [0x1E] = "11", [0x1F] = "12", [0x20] = "0", [0x21] = "1", [0x22] = "2", [0x23] = "3",
        [0x24] = "4", [0x25] = "5", [0x26] = "6", [0x27] = "7", [0x28] = "8", [0x29] = "9",
        [0x2A] = "DOT", [0x2B] = "ENTER", [0x2C] = "CLEAR", [0x2F] = "NEXT FAVORITE",
        [0x30] = "CHANNEL UP", [0x31] = "CHANNEL DOWN", [0x32] = "PREVIOUS CHANNEL",
        [0x33] = "SOUND SELECT", [0x34] = "INPUT SELECT", [0x35] = "INFO", [0x36] = "HELP",
        [0x37] = "PAGE UP", [0x38] = "PAGE DOWN", [0x40] = "POWER", [0x41] = "VOLUME UP",
        [0x42] = "VOLUME DOWN", [0x43] = "MUTE", [0x44] = "PLAY", [0x45] = "STOP", [0x46] = "PAUSE",
        [0x47] = "RECORD", [0x48] = "REWIND", [0x49] = "FAST FORWARD", [0x4A] = "EJECT",
        [0x4B] = "NEXT", [0x4C] = "PREVIOUS", [0x4D] = "STOP RECORD", [0x4E] = "PAUSE RECORD",
        [0x50] = "ANGLE", [0x51] = "SUB PICTURE", [0x52] = "VIDEO ON DEMAND", [0x53] = "GUIDE",
        [0x54] = "TIMER", [0x55] = "INITIAL CONFIG", [0x56] = "BROADCAST TYPE", [0x57] = "SOUND MODE",
        [0x60] = "PLAY (FN)", [0x61] = "PLAY / PAUSE", [0x62] = "RECORD (FN)", [0x63] = "PAUSE RECORD (FN)",
        [0x64] = "STOP (FN)", [0x65] = "MUTE (FN)", [0x66] = "RESTORE VOLUME", [0x67] = "TUNE",
        [0x68] = "SELECT MEDIA", [0x69] = "SELECT AV INPUT", [0x6A] = "SELECT AUDIO INPUT",
        [0x6B] = "POWER TOGGLE", [0x6C] = "POWER OFF", [0x6D] = "POWER ON", [0x71] = "BLUE",
        [0x72] = "RED", [0x73] = "GREEN", [0x74] = "YELLOW", [0x75] = "F5", [0x76] = "DATA",
        [0x91] = "RETURN (SAMSUNG)",
    };
    return (key < sizeof kNames / sizeof kNames[0] && kNames[key]) ? kNames[key] : "Undefined";
}

// <Play> (0x41) and <Deck Control> (0x42) as the key code a remote would have sent, or 0xFF.
static uint8_t transport_key(uint8_t op, uint8_t mode)
{
    if (op == 0x41) {
        if (mode == 0x24) return 0x44;                    // play forward -> PLAY
        if (mode == 0x25) return 0x46;                    // play still -> PAUSE
        if (mode >= 0x05 && mode <= 0x07) return 0x49;    // fast forward -> >>
        if (mode >= 0x09 && mode <= 0x0B) return 0x48;    // fast reverse -> <<
        if (mode == 0x20) return 0x48;                    // play reverse -> <<
    } else if (op == 0x42) {
        if (mode == 0x01) return 0x4B;                    // skip forward -> >>|
        if (mode == 0x02) return 0x4C;                    // skip reverse -> |<<
        if (mode == 0x03) return 0x45;                    // stop -> STOP
    }
    return 0xFF;
}

// CEC opcodes by name, for the log.
static const char *get_op_name(uint8_t op)
{
    switch (op) {
        case 0x00: return "FEATURE ABORT";
        case 0x04: return "IMAGE VIEW ON";
        case 0x0D: return "TEXT VIEW ON";
        case 0x1A: return "GIVE DECK STATUS";
        case 0x1B: return "DECK STATUS";
        case 0x32: return "SET MENU LANGUAGE";
        case 0x36: return "STANDBY";
        case 0x41: return "PLAY";
        case 0x42: return "DECK CONTROL";
        case 0x46: return "GIVE OSD NAME";
        case 0x47: return "SET OSD NAME";
        case 0x70: return "SYSTEM AUDIO MODE REQUEST";
        case 0x71: return "GIVE AUDIO STATUS";
        case 0x72: return "SET SYSTEM AUDIO MODE";
        case 0x7A: return "REPORT AUDIO STATUS";
        case 0x7D: return "GIVE SYSTEM AUDIO STATUS";
        case 0x7E: return "SYSTEM AUDIO STATUS";
        case 0x80: return "ROUTING CHANGE";
        case 0x81: return "ROUTING INFORMATION";
        case 0x82: return "ACTIVE SOURCE";
        case 0x83: return "GIVE PHYSICAL ADDRESS";
        case 0x84: return "REPORT PHYSICAL ADDRESS";
        case 0x85: return "REQUEST ACTIVE SOURCE";
        case 0x86: return "SET STREAM PATH";
        case 0x87: return "DEVICE VENDOR ID";
        case 0x89: return "VENDOR COMMAND";
        case 0x8C: return "GIVE VENDOR ID";
        case 0x8D: return "MENU REQUEST";
        case 0x8E: return "MENU STATUS";
        case 0x8F: return "GIVE POWER STATUS";
        case 0x90: return "REPORT POWER STATUS";
        case 0x91: return "GET MENU LANGUAGE";
        case 0x9D: return "INACTIVE SOURCE";
        case 0x9E: return "CEC VERSION";
        case 0x9F: return "GET CEC VERSION";
        case 0xA0: return "VENDOR COMMAND WITH ID";
        case 0xFF: return "ABORT";
        default:   return "FRAME";
    }
}

static const char *get_la_name(uint8_t la)
{
    switch (la) {
        case 0x0: return "TV";
        case 0x1: return "Recording 1";
        case 0x2: return "Recording 2";
        case 0x3: return "Tuner 1";
        case 0x4: return "Playback (Xbox)";
        case 0x5: return "Audio System";
        case 0x6: return "Tuner 2";
        case 0x7: return "Tuner 3";
        case 0x8: return "Playback 2";
        case 0x9: return "Recording 3";
        case 0xA: return "Tuner 4";
        case 0xB: return "Playback 3";
        case 0xC: return "Backup 1";
        case 0xD: return "Backup 2";
        case 0xE: return "Specific Device";
        case 0xF: return "Broadcast";
        default:  return "Unknown";
    }
}

static void log_event(const char *text, DWORD color)
{
    if (s_log_count < MAX_CEC_LOG_ENTRIES) {
        snprintf(s_log[s_log_count].text, sizeof(s_log[s_log_count].text), "%s", text);
        s_log[s_log_count].color = color;
        s_log_count++;
    } else {
        for (int i = 0; i < MAX_CEC_LOG_ENTRIES - 1; i++) {
            s_log[i] = s_log[i + 1];
        }
        snprintf(s_log[MAX_CEC_LOG_ENTRIES - 1].text, sizeof(s_log[MAX_CEC_LOG_ENTRIES - 1].text), "%s", text);
        s_log[MAX_CEC_LOG_ENTRIES - 1].color = color;
    }
}

void CECRemote_Init(void)
{
    memset(s_highlight_timer, 0, sizeof(s_highlight_timer));
    s_last_key_code = 0xFF;
    snprintf(s_last_key_name, sizeof(s_last_key_name), "%s", "None");
    snprintf(s_last_action_name, sizeof(s_last_action_name), "%s", "Waiting for TV remote...");
    snprintf(s_last_raw_hex, sizeof(s_last_raw_hex), "%s", "None");
    s_total_frames = 0;
    s_key_is_pressed = false;
    s_held_key = 0xFF;
    s_log_count = 0;
    s_testing = false;
}

bool CECRemote_IsTesting(void) { return s_testing; }

bool CECRemote_Update(bool on_tab, bool may_poll)
{
    bool dirty = false;

    // Decay visual button highlight timers every frame (60 Hz)
    for (int i = 0; i < 256; i++) {
        if (s_highlight_timer[i] > 0) {
            s_highlight_timer[i]--;
            dirty = true;
        }
    }

    if (!on_tab && s_testing) {
        s_testing = false;
        dirty = true;
    }
    if (!may_poll) {
        memset(&s_nav, 0, sizeof(s_nav));
        return dirty;
    }

    DWORD now = GetTickCount();
    if (s_testing && s_key_is_pressed && s_held_key == 0x0D && now - s_back_since >= TEST_STOP_HOLD_MS) {
        s_testing = false;
        log_event("Test ended (remote BACK held)", COLOR_PHOSPHOR_AMBER);
        dirty = true;
    }

    // Rate-limit SMBus buffer polling to 100ms
    static DWORD s_last_poll_tick = 0;
    if (now - s_last_poll_tick < 100) {
        return dirty;
    }
    s_last_poll_tick = now;

    // Poll for pending CEC frame over SMBus relay
    uint8_t frame[16];
    uint8_t len = 0;
    if (smbus_poll_cec_frame(frame, &len) && len > 0 && len <= sizeof(frame)) {
        dirty = true;
        s_total_frames++;

        uint8_t src = (frame[0] >> 4) & 0x0F;
        uint8_t dst = frame[0] & 0x0F;
        uint8_t op  = (len >= 2) ? frame[1] : 0;
        uint8_t key = (len >= 3) ? frame[2] : 0xFF;


        // Format raw hex string
        char hex[48] = "";
        for (uint8_t i = 0; i < len && i < 6; i++) {
            char bstr[8];
            snprintf(bstr, sizeof(bstr), "%02X ", frame[i]);
            strncat(hex, bstr, sizeof(hex) - strlen(hex) - 1);
        }
        snprintf(s_last_raw_hex, sizeof(s_last_raw_hex), "%s(%s -> %s)", hex, get_la_name(src), get_la_name(dst));

        // Opcode 0x44: User Control Pressed; 0x8A (Vendor Remote Button Down) carries the same
        // key codes on Samsung sets.
        const bool pressed = (op == 0x44 || op == 0x8A) && len >= 3;
        if (pressed && !s_testing) nav_key(key);
        if (pressed) {
            s_last_key_code = key;
            snprintf(s_last_key_name, sizeof(s_last_key_name), "%s", get_key_name(key));
            s_highlight_timer[key] = 25; // Keep button lit for ~400ms

            snprintf(s_last_action_name, sizeof(s_last_action_name), "PRESSED: %s (0x%02X)", s_last_key_name, key);

            // Debounce: Only log initial press or key transition to prevent log flooding when holding a button
            if (!s_key_is_pressed || key != s_held_key) {
                char logMsg[64];
                snprintf(logMsg, sizeof(logMsg), "0x%02X <%s : %s (0x%02X)>", op,
                         op == 0x44 ? "USER PRESSED" : "VENDOR KEY", s_last_key_name, key);
                log_event(logMsg, COLOR_PHOSPHOR_BRIGHT);
                s_key_is_pressed = true;
                s_held_key = key;
                s_back_since = now;
            }
        }
        // Opcode 0x45: User Control Released (0x8B: the vendor form)
        else if (op == 0x45 || op == 0x8B) {
            if (s_key_is_pressed) {
                s_key_is_pressed = false;
                s_held_key = 0xFF;
                if (s_last_key_code != 0xFF && s_highlight_timer[s_last_key_code] > 6) {
                    s_highlight_timer[s_last_key_code] = 6;
                }
                snprintf(s_last_action_name, sizeof(s_last_action_name), "RELEASED (0x%02X)", op);

                char logMsg[64];
                snprintf(logMsg, sizeof(logMsg), "0x%02X <USER RELEASED>", op);
                log_event(logMsg, COLOR_PHOSPHOR_DIM);
            }
        }
        // Other CEC Frames (Active Source, Routing, Standby, etc.)
        else {
            const char *opName = (len >= 2) ? get_op_name(op) : "POLL";

            snprintf(s_last_action_name, sizeof(s_last_action_name), "RECVD: <%s> (0x%02X)", opName, op);

            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "0x%02X <%s> from %s", op, opName, get_la_name(src));
            log_event(logMsg, COLOR_PHOSPHOR_NORMAL);

            // Many TVs send the transport keys as <Play> / <Deck Control> rather than key
            // presses: light the same buttons.
            const uint8_t k = transport_key(op, len >= 3 ? frame[2] : 0xFF);
            if (k != 0xFF) {
                s_highlight_timer[k] = 25;
                snprintf(s_last_action_name, sizeof(s_last_action_name), "%s (as <%s>)", get_key_name(k), opName);
            }
        }
    }
    return dirty;
}

void CECRemote_HandleInput(const input_state_t *in)
{
    if (!in) return;
    if (!s_testing && in->a) {
        s_testing = true;
        log_event("Test started", COLOR_PHOSPHOR_AMBER);
    } else if (s_testing && (in->b || in->back)) {
        s_testing = false;
        log_event("Test ended", COLOR_PHOSPHOR_AMBER);
    }
    if (in->y) {
        s_log_count = 0;
        s_total_frames = 0;
        s_key_is_pressed = false;
        s_held_key = 0xFF;
        snprintf(s_last_action_name, sizeof(s_last_action_name), "%s", "Log reset. Listening for remote...");
        snprintf(s_last_key_name, sizeof(s_last_key_name), "%s", "None");
        snprintf(s_last_raw_hex, sizeof(s_last_raw_hex), "%s", "None");
        memset(s_highlight_timer, 0, sizeof(s_highlight_timer));
    }
}

// Is any of these key codes lit (pressed in the last ~400 ms)?
static bool lit_any(const uint8_t *keys, int n)
{
    for (int i = 0; i < n; i++) {
        if (s_highlight_timer[keys[i]] > 0) return true;
    }
    return false;
}
#define LIT(...) lit_any((const uint8_t[]){ __VA_ARGS__ }, (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

// One remote key: an outlined box, filled while the TV reports it pressed.
static void draw_key(float x, float y, float w, float h, const char *label, bool lit)
{
    if (lit) Terminal_DrawFilledRect(x, y, w, h, COLOR_PHOSPHOR_BRIGHT);
    Terminal_DrawRect(x, y, w, h, lit ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM);
    const float tw = (float)Terminal_TextWidth(label);
    Terminal_PrintAt((int)(x + (w - tw) / 2.0f), (int)(y + 2.0f), lit ? 0xFF000000 : COLOR_PHOSPHOR_NORMAL, "%s", label);
}

// The keys a TV can pass to a playback device. Power, volume, mute and input have CEC codes
// too, but TVs act on those themselves and never forward them (a TV going to standby says so
// with <Standby>, which shows in the log), so they aren't drawn.
static void draw_remote(float x0, float y0, float w, float h)
{
    Terminal_DrawRectThick(x0, y0, w, h, COLOR_PHOSPHOR_DIM);
    const float kh = 30.0f;
#define K(dx, dy, kw, label, lit) draw_key(x0 + (dx), y0 + (dy), (kw), kh, (label), (lit))

    K(20, 12, 100, "MENU", LIT(0x09, 0x0A, 0x0B, 0x10, 0x11));
    K(130, 12, 100, "INFO", LIT(0x35));
    K(240, 12, 100, "BACK", LIT(0x0D, 0x91));

    K(145, 56, 70, "^", LIT(0x01));
    K(65, 92, 70, "<", LIT(0x03));
    K(145, 92, 70, "OK", LIT(0x00, 0x2B));
    K(225, 92, 70, ">", LIT(0x04));
    K(145, 128, 70, "v", LIT(0x02));

    K(20, 176, 78, "<<", LIT(0x48));
    K(101, 176, 78, "PLAY", LIT(0x44, 0x60, 0x61));
    K(182, 176, 78, "PAUSE", LIT(0x46, 0x61));
    K(263, 176, 78, ">>", LIT(0x49));
    K(20, 212, 58, "|<<", LIT(0x4C));
    K(85, 212, 58, "STOP", LIT(0x45, 0x64));
    K(150, 212, 58, ">>|", LIT(0x4B));
    K(215, 212, 58, "CH+", LIT(0x30));
    K(280, 212, 58, "CH-", LIT(0x31));

    // The four colour keys in their colours.
    static const uint8_t kColourKeys[4] = { 0x72, 0x73, 0x74, 0x71 };   // red, green, yellow, blue
    static const DWORD kDim[4]    = { 0xFF601010, 0xFF106010, 0xFF606010, 0xFF101060 };
    static const DWORD kBright[4] = { 0xFFFF3030, 0xFF30FF30, 0xFFFFFF30, 0xFF4040FF };
    for (int i = 0; i < 4; i++) {
        const bool on = s_highlight_timer[kColourKeys[i]] > 0;
        Terminal_DrawFilledRect(x0 + 20.0f + (float)i * 82.0f, y0 + 258.0f, 74.0f, 16.0f, on ? kBright[i] : kDim[i]);
    }

    static const char *const kDigits[10] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
    for (int i = 0; i < 10; i++) {
        const uint8_t code = (uint8_t)(i == 9 ? 0x20 : 0x21 + i);   // 0x20 is "0"
        K(20.0f + (float)(i % 5) * 64.0f, 290.0f + (float)(i / 5) * 36.0f, 56, kDigits[i], s_highlight_timer[code] > 0);
    }
#undef K
}

void CECRemote_Render(void)
{
    const int ml = Terminal_GetLeftMargin();
    const int mt = Terminal_GetTopMargin();
    const int lh = Terminal_GetLineHeight();

    const float rx = (float)ml, ry = (float)(mt + 5 * lh);
    draw_remote(rx, ry, 360.0f, 370.0f);

    // Right of the remote: the test, the bus, the traffic.
    const int ix = 400;
    int row = 5;
    if (s_testing) {
        Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_AMBER, "REMOTE TEST: ON -- keys only light up; hold remote BACK to end");
    } else {
        Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "REMOTE TEST: OFF -- (A) or the remote's OK starts it");
    }
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "  Last key  : %s", s_last_action_name);
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "  Raw frame : %s", s_last_raw_hex);

    // The bus: polled while the tab is drawn (the cursor blink redraws it twice a second).
    static uint8_t s_st = 0;
    static DWORD s_last_st = 0;
    const DWORD now = GetTickCount();
    if (now - s_last_st >= 150) {
        s_last_st = now;
        smbus_read_cec_status(&s_st);
    }
    row++;
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_DIM, "CEC BUS:");
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "  Sending   : %s | Arbitration: %s",
                            (s_st & XHD_CEC_STATUS_TX_BUSY) ? "BUSY" : "IDLE",
                            (s_st & XHD_CEC_STATUS_TX_LOST_ARB) ? "LOST (another device talked)" : "OK");
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "  Last frame: %s",
                            (s_st & XHD_CEC_STATUS_LAST_TX_ACKED) ? "acknowledged by the TV" :
                            (s_st & XHD_CEC_STATUS_TX_TIMEOUT) ? "NOT acknowledged (retries timed out)" : "none yet / pending");
    // The remote handling reads every frame as it arrives, on every tab, so the board's
    // pending bit is practically never seen; count what came in instead.
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_NORMAL, "  From TV   : %u frames since this app started", s_total_frames);

    row++;
    Terminal_PrintRowIndent(row++, ix, COLOR_PHOSPHOR_DIM, "RECENT CEC TRAFFIC:");
    #define LOG_SHOWN 4
    const int first = s_log_count > LOG_SHOWN ? s_log_count - LOG_SHOWN : 0;
    if (s_log_count == 0) {
        Terminal_PrintRowIndent(row, ix, COLOR_PHOSPHOR_DARK, "  (nothing received yet)");
    }
    for (int i = first; i < s_log_count; i++) {
        Terminal_PrintRowIndent(row++, ix, s_log[i].color, "  %s", s_log[i].text);
    }

    const int note = Terminal_FooterTopRow() - 2;
    Terminal_PrintRowIndent(note, ix, COLOR_PHOSPHOR_DIM, "Which keys reach the console is up to the TV, and varies by");
    Terminal_PrintRowIndent(note + 1, ix, COLOR_PHOSPHOR_DIM, "make and model. Power, volume and input stay with the TV.");
}
