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
static uint8_t  s_last_opcode = 0;
static uint8_t  s_last_key_code = 0xFF;
static char     s_last_key_name[32] = "None";
static char     s_last_action_name[48] = "Waiting for TV remote...";
static char     s_last_raw_hex[48] = "None";
static uint32_t s_total_frames = 0;
static bool     s_key_is_pressed = false;
static uint8_t  s_held_key = 0xFF;

static cec_log_entry_t s_log[MAX_CEC_LOG_ENTRIES];
static int             s_log_count = 0;

static const char *get_key_name(uint8_t key)
{
    switch (key) {
        case 0x00: return "SELECT / OK";
        case 0x31: return "SELECT / OK";
        case 0x01: return "UP";
        case 0x02: return "DOWN";
        case 0x03: return "LEFT";
        case 0x04: return "RIGHT";
        case 0x09: return "MENU";
        case 0x0A: return "MENU";
        case 0x0D: return "BACK";
        case 0x35: return "INFO";
        case 0x40: return "POWER";
        case 0x6B: return "POWER";
        case 0x6D: return "POWER";
        case 0x44: return "PLAY / PAUSE";
        case 0x46: return "PLAY / PAUSE";
        case 0x60: return "PLAY / PAUSE";
        default:   return "Undefined";
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
    s_last_opcode = 0;
    s_last_key_code = 0xFF;
    snprintf(s_last_key_name, sizeof(s_last_key_name), "%s", "None");
    snprintf(s_last_action_name, sizeof(s_last_action_name), "%s", "Waiting for TV remote...");
    snprintf(s_last_raw_hex, sizeof(s_last_raw_hex), "%s", "None");
    s_total_frames = 0;
    s_key_is_pressed = false;
    s_held_key = 0xFF;
    s_log_count = 0;
}

bool CECRemote_Update(bool isActive)
{
    bool dirty = false;

    // Decay visual button highlight timers every frame (60 Hz)
    for (int i = 0; i < 256; i++) {
        if (s_highlight_timer[i] > 0) {
            s_highlight_timer[i]--;
            dirty = true;
        }
    }

    if (!isActive) {
        return dirty;
    }

    // Rate-limit SMBus buffer polling to 100ms
    static DWORD s_last_poll_tick = 0;
    DWORD now = GetTickCount();
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

        s_last_opcode = op;

        // Format raw hex string
        char hex[48] = "";
        for (uint8_t i = 0; i < len && i < 6; i++) {
            char bstr[8];
            snprintf(bstr, sizeof(bstr), "%02X ", frame[i]);
            strncat(hex, bstr, sizeof(hex) - strlen(hex) - 1);
        }
        snprintf(s_last_raw_hex, sizeof(s_last_raw_hex), "%s(%s -> %s)", hex, get_la_name(src), get_la_name(dst));

        // Opcode 0x44: User Control Pressed
        if (op == 0x44 && len >= 3) {
            s_last_key_code = key;
            snprintf(s_last_key_name, sizeof(s_last_key_name), "%s", get_key_name(key));
            s_highlight_timer[key] = 25; // Keep button lit for ~400ms

            snprintf(s_last_action_name, sizeof(s_last_action_name), "PRESSED: %s (0x%02X)", s_last_key_name, key);

            // Debounce: Only log initial press or key transition to prevent log flooding when holding a button
            if (!s_key_is_pressed || key != s_held_key) {
                char logMsg[64];
                snprintf(logMsg, sizeof(logMsg), "0x44 <USER PRESSED : %s (0x%02X)>", s_last_key_name, key);
                log_event(logMsg, COLOR_PHOSPHOR_BRIGHT);
                s_key_is_pressed = true;
                s_held_key = key;
            }
        }
        // Opcode 0x45: User Control Released
        else if (op == 0x45) {
            if (s_key_is_pressed) {
                s_key_is_pressed = false;
                s_held_key = 0xFF;
                if (s_last_key_code != 0xFF && s_highlight_timer[s_last_key_code] > 6) {
                    s_highlight_timer[s_last_key_code] = 6;
                }
                snprintf(s_last_action_name, sizeof(s_last_action_name), "RELEASED (0x45)");

                char logMsg[64];
                snprintf(logMsg, sizeof(logMsg), "0x45 <USER RELEASED>");
                log_event(logMsg, COLOR_PHOSPHOR_DIM);
            }
        }
        // Other CEC Frames (Active Source, Routing, Standby, etc.)
        else {
            const char *opName = "FRAME";
            if (op == 0x82) opName = "ACTIVE SOURCE";
            else if (op == 0x04) opName = "IMAGE VIEW ON";
            else if (op == 0x36) opName = "STANDBY";
            else if (op == 0x80) opName = "ROUTING CHANGE";
            else if (op == 0x86) opName = "SET STREAM PATH";
            else if (op == 0x46) opName = "GIVE OSD NAME";
            else if (op == 0x83) opName = "GIVE PHYS ADDR";

            snprintf(s_last_action_name, sizeof(s_last_action_name), "RECVD: <%s> (0x%02X)", opName, op);

            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "0x%02X <%s> from %s", op, opName, get_la_name(src));
            log_event(logMsg, COLOR_PHOSPHOR_NORMAL);
        }
    }
    return dirty;
}

void CECRemote_HandleInput(const input_state_t *in)
{
    if (in && in->y) {
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

static DWORD get_btn_color(uint8_t key)
{
    return (s_highlight_timer[key] > 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;
}

void CECRemote_Render(void)
{
    int ml = Terminal_GetLeftMargin();
    int mt = Terminal_GetTopMargin();
    int lh = Terminal_GetLineHeight();

    // Hardware 2D vector rectangular remote outline (Width: 420px, Height: covers rows 6-14)
    float boxX = (float)ml;
    float boxY = (float)(mt + 6 * lh - 8);
    float boxW = 420.0f;
    float boxH = (float)(8 * lh + 22);

    Terminal_DrawRectThick(boxX, boxY, boxW, boxH, COLOR_PHOSPHOR_DIM);

    // Inner dividing line under remote header
    Terminal_DrawLine(boxX, (float)(mt + 7 * lh - 6), boxX + boxW, (float)(mt + 7 * lh - 6), COLOR_PHOSPHOR_DARK);

    // Inner dividing line separating D-pad and controls
    Terminal_DrawLine(boxX, (float)(mt + 13 * lh - 6), boxX + boxW, (float)(mt + 13 * lh - 6), COLOR_PHOSPHOR_DARK);

    // Row 6: Title mathematically centered to the box & Right-side Monitor Header
    Terminal_PrintRowCenterIn(6, 0, (int)boxW, COLOR_PHOSPHOR_BRIGHT, "[ VIRTUAL TV REMOTE ]");
    Terminal_PrintRowIndent(6, 470, COLOR_PHOSPHOR_DIM,    "HDMI-CEC BUS METRICS & PROTOCOL MONITOR:");

    // Row 7: Power, Info, Back/Exit mathematically centered across 3 columns
    DWORD pwrColor = (s_highlight_timer[0x40] > 0 || s_highlight_timer[0x6B] > 0 || s_highlight_timer[0x6D] > 0)
                     ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;
    DWORD infoColor = get_btn_color(0x35);
    DWORD backColor = (s_highlight_timer[0x0D] > 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;

    Terminal_PrintRowCenterIn(7, 0, 140,   pwrColor,  "[ PWR ]");
    Terminal_PrintRowCenterIn(7, 140, 140, infoColor, "[ INFO ]");
    Terminal_PrintRowCenterIn(7, 280, 140, backColor, "[ BACK ]");
    Terminal_PrintRowIndent(7, 470, COLOR_PHOSPHOR_NORMAL, "  Bus Status  : ACTIVE (HDMI Pin 13) | Address: 0x4 (Playback)");

    // Row 8:
    Terminal_PrintRowIndent(8, 470, COLOR_PHOSPHOR_NORMAL, "  Last Action : %s", s_last_action_name);

    // Row 9: D-Pad Up mathematically centered to the box
    Terminal_PrintRowCenterIn(9, 0, (int)boxW, get_btn_color(0x01), (s_highlight_timer[0x01] > 0) ? "[ *^ ]" : "[  ^  ]");
    Terminal_PrintRowIndent(9, 470, COLOR_PHOSPHOR_NORMAL, "  Raw Frame   : %s", s_last_raw_hex);

    // Row 10: D-Pad Left, OK, Right centered in their columns
    DWORD okColor = (s_highlight_timer[0x00] > 0 || s_highlight_timer[0x31] > 0)
                    ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;

    Terminal_PrintRowCenterIn(10, 0, 140,   get_btn_color(0x03), (s_highlight_timer[0x03] > 0) ? "[ *< ]" : "[  <  ]");
    Terminal_PrintRowCenterIn(10, 140, 140, okColor,             (okColor == COLOR_PHOSPHOR_BRIGHT) ? "[*OK*]" : "[ OK ]");
    Terminal_PrintRowCenterIn(10, 280, 140, get_btn_color(0x04), (s_highlight_timer[0x04] > 0) ? "[ >* ]" : "[  >  ]");
    Terminal_PrintRowIndent(10, 470, COLOR_PHOSPHOR_NORMAL, "  Traffic     : %u total packets received from TV", s_total_frames);

    // Row 11: D-Pad Down mathematically centered to the box
    Terminal_PrintRowCenterIn(11, 0, (int)boxW, get_btn_color(0x02), (s_highlight_timer[0x02] > 0) ? "[ *v ]" : "[  v  ]");
    Terminal_PrintRowIndent(11, 470, COLOR_PHOSPHOR_DARK,  "----------------------------------------------------------------");

    // Row 12: Event Log Header
    Terminal_PrintRowIndent(12, 470, COLOR_PHOSPHOR_DIM,   "RECENT CEC BUS TRAFFIC LOG (DEBOUNCED):");

    // Row 13: Menu and Play/Pause centered in left/right halves of remote
    DWORD menuColor = (s_highlight_timer[0x09] > 0 || s_highlight_timer[0x0A] > 0)
                      ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;
    DWORD playPauseColor = (s_highlight_timer[0x44] > 0 ||
                            s_highlight_timer[0x46] > 0 || s_highlight_timer[0x60] > 0)
                           ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM;

    Terminal_PrintRowCenterIn(13, 0, 210,   menuColor,      (menuColor == COLOR_PHOSPHOR_BRIGHT) ? "[ *MENU* ]" : "[  MENU  ]");
    Terminal_PrintRowCenterIn(13, 210, 210, playPauseColor, (playPauseColor == COLOR_PHOSPHOR_BRIGHT) ? "[ *PLAY / PAUSE* ]" : "[ PLAY / PAUSE ]");
    if (s_log_count > 0) {
        Terminal_PrintRowIndent(13, 470, s_log[0].color,   "  1. %s", s_log[0].text);
    } else {
        Terminal_PrintRowIndent(13, 470, COLOR_PHOSPHOR_DARK, "  (No CEC traffic recorded yet)");
    }

    // Row 14: Log line 2
    if (s_log_count > 1) {
        Terminal_PrintRowIndent(14, 470, s_log[1].color,   "  2. %s", s_log[1].text);
    }

    // Row 15: Log line 3
    if (s_log_count > 2) {
        Terminal_PrintRowIndent(15, 470, s_log[2].color,   "  3. %s", s_log[2].text);
    }

    // Row 16: Log line 4
    if (s_log_count > 3) {
        Terminal_PrintRowIndent(16, 470, s_log[3].color,   "  4. %s", s_log[3].text);
    }

    // Row 17: Log line 5
    if (s_log_count > 4) {
        Terminal_PrintRowIndent(17, 470, s_log[4].color,   "  5. %s", s_log[4].text);
    }

    // Row 18: Log line 6
    if (s_log_count > 5) {
        Terminal_PrintRowIndent(18, 470, s_log[5].color,   "  6. %s", s_log[5].text);
    }

    // Row 19: Log line 7
    if (s_log_count > 6) {
        Terminal_PrintRowIndent(19, 470, s_log[6].color,   "  7. %s", s_log[6].text);
    }
}
