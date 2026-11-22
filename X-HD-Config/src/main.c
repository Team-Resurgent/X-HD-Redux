// SPDX-License-Identifier: GPL-3.0-only
#include "xbox_base.h"
#include "defines.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include "smbus_hal.h"
#include "input.h"
#include "ui_terminal.h"
#include "ui_osk.h"
#include "ui_edid.h"
#include "ui_about.h"
#include "ui_cec_remote.h"
#include "video_mode_mgr.h"
#include "xbox_eeprom.h"
#include "ui_pattern.h"
#include "ui_firmware.h"
#include "app_dir.h"
#include "ui_picklist.h"
#include "spd_presets.h"
#include "settings_names.h"
#include "confirm.h"
#include "fw_update.h"
#include "app.h"

// The tabs, in WHITE / BLACK order. Their on-screen numbers are these plus one.
typedef enum {
    TAB_VIDEO,
    TAB_POSITION,
    TAB_PICTURE,
    TAB_CEC,
    TAB_STATUS,
    TAB_EDID,
    TAB_EEPROM,
    TAB_FIRMWARE,
    TAB_HELP,
    NUM_TABS
} tab_id_t;

// Application State
typedef struct {
    tab_id_t active_tab;
    int selected_item[NUM_TABS];
    char status_msg[128];

    // Board detection: xhd_present only for a running X-HD application -- a board in update
    // mode answers the version opcodes too, but none of the settings ones.
    fw_board_t board;
    bool xhd_present;
    char fw_ver_str[32];

    // Which of the cached blocks below the last read actually filled. A block that failed
    // shows as "not read" instead of stale or zeroed values passed off as the board's.
    bool vi_ok;
    bool tlm_ok;
    bool about_ok;
    bool cfg_ok;
    bool pic_ok;        // also false on firmware from before the picture controls

    // Cached live structures
    XHD_VIDEO_INFO vi;
    XHD_TELEMETRY  tlm;
    XHD_ABOUT      about;
    xhd_user_config cfg;
    char           spd_name[25];
    XHD_PICTURE_SET pic;     // the live profile and all three

    // Local live settings
    uint8_t color_space;
    uint8_t aspect_ratio;
    uint8_t scan_mode;
    uint8_t wire_vic_mode;
    bool    sd_exact;
    uint8_t clock_phase;
    int8_t  pos_h;
    int8_t  pos_v;
    bool    cec_clock;
    bool    cec_one_touch;
    uint8_t cec_standby_mode;   // 0=Restore, 1=Off, 2=Always standby (cfg.cec_standby_off)
    bool    audio_mute;
    bool    smbus_freeze;

    // Live tab polling
    DWORD   last_poll_tick;
} app_state_t;

static app_state_t g_state;

static char s_customSpd[25] = "";
static bool s_hasCustomSpd = false;
static int  s_dirty_frames = 2;

// The status line. A plain message goes after STATUS_INFO_MS; an alert (an error, or the
// outcome of an action like a save or the EEPROM write) stays until the next button press,
// so it isn't missed while looking at the TV.
#define STATUS_INFO_MS 3000
static DWORD s_status_expire_tick = 0;
static bool  s_status_alert = false;

static void show_status(const char *msg, bool alert)
{
    s_dirty_frames = 2;
    s_status_alert = false;
    s_status_expire_tick = 0;
    if (!msg || msg[0] == '\0') {
        g_state.status_msg[0] = '\0';
        return;
    }
    strncpy(g_state.status_msg, msg, sizeof(g_state.status_msg) - 1);
    g_state.status_msg[sizeof(g_state.status_msg) - 1] = '\0';
    s_status_alert = alert;
    if (!alert) {
        s_status_expire_tick = GetTickCount() + STATUS_INFO_MS;
    }
}

static void set_status(const char *msg) { show_status(msg, false); }
static void set_alert(const char *msg)  { show_status(msg, true); }

// Settings changed on the board since the last save (or reload, or the board restarting).
// Tracked here, not read from the board: the firmware has no "differs from flash" flag, so
// changes made by an earlier run of this app that exited without saving aren't seen.
static bool s_unsaved = false;

// Two-press actions (Tabs 3 and 7); a confirmation lapses after CONFIRM_TIMEOUT_MS.
enum { CONF_EEPROM, CONF_DEFAULTS, CONF_EEPROM_REBOOT, CONF_PIC_COPY };
static confirm_t s_confirm = CONFIRM_NONE;

static void cancel_confirms(void)
{
    confirm_clear(&s_confirm);
}

#define WRITE_FAILED "ERROR: SMBus write failed (board not responding)"

// The result of writing a setting the board saves (page 63): on success its message and the
// unsaved flag; on failure an alert. Returns `ok`, so the caller keeps its copy of the value
// only when the board took it.
static bool setting_written(bool ok, const char *msg)
{
    if (ok) {
        s_unsaved = true;
        set_status(msg);
    } else {
        set_alert(WRITE_FAILED);
    }
    return ok;
}

// The same for a live-only setting (not saved by the board: audio mute).
static bool live_written(bool ok, const char *msg)
{
    if (ok) set_status(msg); else set_alert(WRITE_FAILED);
    return ok;
}

static void pic_sync_edit(void);   // Tab 3

// Right after a firmware update the new application is still starting, so a failed video-info
// read isn't reported: the live tabs' 1 Hz poll reads it again.
static bool s_just_flashed = false;

static void refresh_all_from_board(void)
{
    smbus_detect(&g_state.board);
    g_state.xhd_present = g_state.board.kind == FW_BOARD_XHD_APP;
    g_state.vi_ok = g_state.tlm_ok = g_state.about_ok = g_state.cfg_ok = g_state.pic_ok = false;
    if (!g_state.xhd_present) {
        switch (g_state.board.kind) {
        case FW_BOARD_XHD_BOOT: set_status("Board is in update mode: finish the update on Tab 8"); break;
        case FW_BOARD_HDPLUS:   set_status("Stock HD+ found: Tab 8 converts it to X-HD"); break;
        default:                set_alert("No board answers at SMBus 0xD2"); break;
        }
        snprintf(g_state.fw_ver_str, sizeof(g_state.fw_ver_str), "N/A");
        return;
    }

    g_state.vi_ok = smbus_read_video_info(&g_state.vi);
    g_state.tlm_ok = smbus_read_telemetry(&g_state.tlm);
    g_state.about_ok = smbus_read_about(&g_state.about);
    if (!smbus_read_spd(g_state.spd_name)) {
        g_state.spd_name[0] = '\0';
    }
    if (g_state.spd_name[0] != '\0') {
        bool matchesPreset = false;
        for (int i = 0; i < SPD_PRESET_COUNT; i++) {
            char preset[25];
            SpdPreset_Name(i, preset);
            if (strncmp(g_state.spd_name, preset, 24) == 0) {
                matchesPreset = true;
                break;
            }
        }
        if (!matchesPreset) {
            strncpy(s_customSpd, g_state.spd_name, sizeof(s_customSpd) - 1);
            s_customSpd[sizeof(s_customSpd) - 1] = '\0';
            s_hasCustomSpd = true;
        }
    }
    EDID_RefreshFromBoard();
    EEPROM_Refresh(EDID_GetParsed());

    // Opcode 1 is always 0; 2-4 are major.minor.patch.
    snprintf(g_state.fw_ver_str, sizeof(g_state.fw_ver_str), "%u.%u.%u",
             g_state.board.ver[1], g_state.board.ver[2], g_state.board.ver[3]);

    g_state.cfg_ok = smbus_read_user_config(&g_state.cfg);
    // Not counted as a failed read: older firmware simply has no picture controls.
    g_state.pic_ok = smbus_read_picture(&g_state.pic);
    pic_sync_edit();
    if (g_state.cfg_ok) {
        // What the app shows is the board's own again (after a reload, a restore, a restart).
        s_unsaved = false;
        g_state.color_space   = g_state.cfg.color_space;
        g_state.aspect_ratio  = g_state.cfg.aspect_ratio;
        g_state.scan_mode     = g_state.cfg.scan_mode;
        g_state.wire_vic_mode = g_state.cfg.wire_vic_mode;
        g_state.sd_exact      = (g_state.cfg.sd_exact == 1);
        g_state.cec_clock     = (g_state.cfg.cec_clock_off == 0);
        g_state.cec_one_touch = (g_state.cfg.cec_one_touch_off == 0);
        g_state.cec_standby_mode = (g_state.cfg.cec_standby_off <= 2) ? g_state.cfg.cec_standby_off : 0;
        g_state.smbus_freeze  = (g_state.cfg.smbus_freeze == 1);

        uint8_t enc = g_state.cfg.active_encoder;
        if (enc < 3) {
            g_state.clock_phase = g_state.cfg.enc_cal[enc].clock_phase;
        }
    }

    if (g_state.vi_ok) {
        g_state.pos_h = g_state.vi.pos_h;
        g_state.pos_v = g_state.vi.pos_v;
    }

    uint8_t flags = 0;
    if (smbus_read_live_flags(&flags)) {
        g_state.audio_mute   = (flags & XHD_LIVE_FLAG_AUDIO_MUTE_HELD) != 0;
        g_state.smbus_freeze = (flags & XHD_LIVE_FLAG_FREEZE_ON) != 0;
    }

    // Say which reads failed rather than claiming everything loaded.
    char failed[64] = "";
    if (!g_state.cfg_ok)   strcat(failed, " settings");
    if (!g_state.vi_ok && !s_just_flashed) strcat(failed, " video-info");
    if (!g_state.tlm_ok)   strcat(failed, " telemetry");
    if (!g_state.about_ok) strcat(failed, " about");
    if (failed[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERROR: could not read from the board:%s -- (Y) on Tab 1 retries", failed);
        set_alert(msg);
    } else {
        set_status("Loaded settings & telemetry from the board");
    }
}

// ----------------------------------------------------------------------------
// Settings lists: one row per item, "> Label   value"
// ----------------------------------------------------------------------------

#define LIST_FIRST_ROW 7   // tabs 1, 3 and 7 list their items from row 7

// A settings tab's heading row; when the board's settings couldn't be read, it says so in
// amber instead, on the same row so the items below don't move.
static void settings_heading(int row, const char *heading)
{
    if (g_state.xhd_present && !g_state.cfg_ok) {
        Terminal_PrintRow(row, COLOR_PHOSPHOR_AMBER,
                          "SETTINGS NOT READ FROM THE BOARD -- values below may not be its own; (Y) retries");
    } else {
        Terminal_PrintRow(row, COLOR_PHOSPHOR_DIM, "%s", heading);
    }
}

// Where a list opened from `label`'s row starts: under the value, past "> Label: ".
static int value_indent(const char *label)
{
    return Terminal_TextWidth("> ") + Terminal_TextWidth(label) + Terminal_TextWidth(": ");
}

// ---------------------------------------------------------------- Tab 1: Settings (video and HDMI-CEC)

// What CEC does when the console powers off, by cfg.cec_standby_off value.
static const char *const kCecStandbyNames[3] = {
    "[RESTORE - TV back as power-on found it]", "[DISABLED - TV left alone]", "[ALWAYS STANDBY]",
};

// ADV7511 0xBA[7:5]: the input clock delay, 0.4 ns a step from -1.2 ns (0) to +1.6 ns (7).
static const char *clock_phase_str(uint8_t phase)
{
    static const char *const kPhases[XHD_CLOCK_PHASE_COUNT] = {
        "0  (-1.2 ns)", "1  (-0.8 ns)", "2  (-0.4 ns)", "3  (0.0 ns, no delay)",
        "4  (+0.4 ns)", "5  (+0.8 ns)", "6  (+1.2 ns)", "7  (+1.6 ns)",
    };
    return phase < XHD_CLOCK_PHASE_COUNT ? kPhases[phase] : "Unknown";
}

// Every Tab 1 item; the main page and the Advanced page each list some of them, in their order.
enum {
    V_COLOR, V_ASPECT, V_SCAN, V_SD_EXACT, V_ONE_TOUCH, V_STANDBY, V_SPD, V_ADVANCED,
    V_WIRE_VIC, V_PHASE, V_CEC_CLOCK, V_FREEZE, V_AUDIO, V_ADV_BACK, V_ITEMS
};
static const char *const kSettingLabels[V_ITEMS] = {
    "Colour Space", "Aspect Ratio", "Scan Mode", "SD 640 Modes", "One Touch Play", "TV at Power-Off",
    "TV Label (SPD)", "Advanced Settings...",
    "Wire-VIC Mode", "Clock Phase", "CEC Clock Out", "Standalone Freeze", "Audio Output", "Back",
};
// The main page: what most people change. The rest -- fixes for one TV or board, and bench
// tests -- are on the Advanced page, which (A) on its row opens and (B) leaves.
static const uint8_t kSetMain[] = {
    V_COLOR, V_ASPECT, V_SCAN, V_SD_EXACT, V_ONE_TOUCH, V_STANDBY, V_SPD, V_ADVANCED,
};
static const uint8_t kSetAdv[] = {
    V_WIRE_VIC, V_PHASE, V_CEC_CLOCK, V_FREEZE, V_AUDIO, V_ADV_BACK,
};
#define SET_MAIN_COUNT ((int)sizeof(kSetMain))
#define SET_ADV_COUNT  ((int)sizeof(kSetAdv))
#define SET_VALUE_X 240   // the value column, px from the left margin

static bool s_set_adv = false;   // the Advanced page is showing
static int  s_adv_sel = 0;       // its cursor

// Main page: the video settings from LIST_FIRST_ROW, a heading and the HDMI-CEC ones, then a
// gap and the Advanced row. Advanced page: its settings, a gap and Back.
static int setting_row(int item)
{
    for (int i = 0; i < SET_MAIN_COUNT; i++) {
        if (kSetMain[i] == item) {
            return LIST_FIRST_ROW + i + (item >= V_ONE_TOUCH ? 2 : 0) + (item == V_ADVANCED ? 1 : 0);
        }
    }
    for (int i = 0; i < SET_ADV_COUNT; i++) {
        if (kSetAdv[i] == item) {
            return LIST_FIRST_ROW + i + (item == V_ADV_BACK ? 1 : 0);
        }
    }
    return LIST_FIRST_ROW;
}

static void render_tab_video(void)
{
    char phase[32], spd[32];
    snprintf(phase, sizeof(phase), "%s", clock_phase_str(g_state.clock_phase));
    snprintf(spd, sizeof(spd), "\"%s\"", g_state.spd_name);
    const char *values[V_ITEMS] = {
        [V_COLOR]     = XhdName_ColorSpace(g_state.color_space),
        [V_ASPECT]    = XhdName_Aspect(g_state.aspect_ratio),
        [V_SCAN]      = XhdName_Scan(g_state.scan_mode),
        [V_SD_EXACT]  = g_state.sd_exact ? "[EXACT - sent as CEA 480p/576p, bars both sides]" : "[FILL - 640 window, no bars]",
        [V_ONE_TOUCH] = g_state.cec_one_touch ? "[ENABLED - Auto-wake & switch TV input]" : "[DISABLED]",
        [V_STANDBY]   = kCecStandbyNames[g_state.cec_standby_mode <= 2 ? g_state.cec_standby_mode : 0],
        [V_SPD]       = spd,
        [V_WIRE_VIC]  = g_state.wire_vic_mode ? "1 (Chip Auto: ADV7511 detection)" : "0 (Strict: CEA VIC only when it matches)",
        [V_PHASE]     = phase,
        [V_CEC_CLOCK] = g_state.cec_clock ? "[ENABLED - Driven on PA8 / Pin 29]" : "[DISABLED]",
        [V_FREEZE]    = g_state.smbus_freeze ? "[ON - console mode packets ignored]" : "[OFF - follows the console]",
        [V_AUDIO]     = g_state.audio_mute ? "[MUTED]" : "[ACTIVE]",
    };
    const uint8_t *list = s_set_adv ? kSetAdv : kSetMain;
    const int count = s_set_adv ? SET_ADV_COUNT : SET_MAIN_COUNT;
    const int sel = s_set_adv ? s_adv_sel : g_state.selected_item[TAB_VIDEO];
    if (s_set_adv) {
        settings_heading(LIST_FIRST_ROW - 1, "ADVANCED (APPLIED LIVE) -- for one TV or board, or testing:");
    } else {
        settings_heading(LIST_FIRST_ROW - 1, "VIDEO (APPLIED LIVE):");
        Terminal_PrintRow(setting_row(V_ONE_TOUCH) - 1, COLOR_PHOSPHOR_DIM, "HDMI-CEC & TV INPUT LABEL:");
    }
    for (int i = 0; i < count; i++) {
        const int item = list[i];
        const int row = setting_row(item);
        Terminal_PrintItem(row, sel == i, "%s", kSettingLabels[item]);
        if (values[item]) {
            Terminal_PrintRowIndent(row, SET_VALUE_X, sel == i ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                                    "%s", values[item]);
        }
    }
}

static void open_value_list(int id, const char *title, const char *const *items, int n, int current, int item)
{
    Picklist_Open(id, title, items, n, current, setting_row(item) + 1, SET_VALUE_X);
}

// The SPD label list: the presets, the custom label if there is one, then the keyboard.
static char s_spdItemText[SPD_PRESET_COUNT + 2][40];
static int  s_spdCustomItem = -1;
static int  s_spdEditItem = 0;

static void open_spd_list(void)
{
    static const char *items[SPD_PRESET_COUNT + 2];
    int n = 0, current = -1;
    for (int i = 0; i < SPD_PRESET_COUNT; i++, n++) {
        char preset[25];
        SpdPreset_Name(i, preset);
        snprintf(s_spdItemText[n], sizeof(s_spdItemText[n]), "\"%s\"", preset);
        if (strncmp(g_state.spd_name, preset, 24) == 0) current = n;
    }
    s_spdCustomItem = -1;
    if (s_hasCustomSpd) {
        snprintf(s_spdItemText[n], sizeof(s_spdItemText[n]), "\"%s\" (custom)", s_customSpd);
        if (current < 0 && strncmp(g_state.spd_name, s_customSpd, 24) == 0) current = n;
        s_spdCustomItem = n++;
    }
    snprintf(s_spdItemText[n], sizeof(s_spdItemText[n]), "Edit with the keyboard...");
    s_spdEditItem = n++;
    for (int i = 0; i < n; i++) items[i] = s_spdItemText[i];
    Picklist_Open(PICK_ID_SPD, "HDMI SPD TV LABEL:", items, n, current, setting_row(V_SPD) + 1, SET_VALUE_X);
}

static void toggle_cec(int item)
{
    switch (item) {
    case V_CEC_CLOCK: {
        const bool v = !g_state.cec_clock;
        if (setting_written(smbus_set_cec_clock(v), v ? "CEC 12MHz Clock Enabled" : "CEC 12MHz Clock Disabled")) {
            g_state.cec_clock = v;
        }
        break;
    }
    case V_ONE_TOUCH: {
        const bool v = !g_state.cec_one_touch;
        if (setting_written(smbus_set_cec_one_touch(v), v ? "CEC One Touch Play Enabled" : "CEC One Touch Play Disabled")) {
            g_state.cec_one_touch = v;
        }
        break;
    }
    }
}

// (A) on a Tab 1 item, from either page.
static void input_settings_item(const input_state_t *in, int item)
{
    static const char *items[PICK_MAX_ITEMS];
    if (!in->a) return;
    switch (item) {
    case V_COLOR:
        for (int i = 0; i < XHD_COLOR_SPACE_COUNT; i++) items[i] = XhdName_ColorSpace((uint8_t)i);
        open_value_list(PICK_ID_COLOR_SPACE, "COLOUR SPACE:", items, XHD_COLOR_SPACE_COUNT, g_state.color_space, item);
        break;
    case V_ASPECT:
        for (int i = 0; i < XHD_ASPECT_COUNT; i++) items[i] = XhdName_Aspect((uint8_t)i);
        open_value_list(PICK_ID_ASPECT, "ASPECT RATIO:", items, XHD_ASPECT_COUNT, g_state.aspect_ratio, item);
        break;
    case V_SCAN:
        for (int i = 0; i < XHD_SCAN_COUNT; i++) items[i] = XhdName_Scan((uint8_t)i);
        open_value_list(PICK_ID_SCAN, "SCAN MODE:", items, XHD_SCAN_COUNT, g_state.scan_mode, item);
        break;
    case V_SD_EXACT: {
        const bool v = !g_state.sd_exact;
        if (setting_written(smbus_set_sd_exact(v), v ? "SD 640 Modes: Exact (CEA 480p/576p)" : "SD 640 Modes: Fill (640 window)")) {
            g_state.sd_exact = v;
        }
        break;
    }
    case V_WIRE_VIC: {
        const uint8_t v = !g_state.wire_vic_mode;
        if (setting_written(smbus_set_wire_vic_mode(v), v ? "Wire-VIC Mode: 1 (Chip Auto)" : "Wire-VIC Mode: 0 (Strict)")) {
            g_state.wire_vic_mode = v;
        }
        break;
    }
    case V_PHASE:
        for (int i = 0; i < XHD_CLOCK_PHASE_COUNT; i++) items[i] = clock_phase_str((uint8_t)i);
        open_value_list(PICK_ID_CLOCK_PHASE, "CLOCK PHASE:", items, XHD_CLOCK_PHASE_COUNT, g_state.clock_phase, item);
        break;
    case V_AUDIO: {
        const bool v = !g_state.audio_mute;
        if (live_written(smbus_set_audio_mute(v), v ? "HDMI Audio Muted (not saved: test only)" : "HDMI Audio Unmuted [Active]")) {
            g_state.audio_mute = v;
        }
        break;
    }
    case V_FREEZE: {
        const bool v = !g_state.smbus_freeze;
        if (setting_written(smbus_set_freeze(v), v ? "Standalone Freeze Enabled" : "Standalone Freeze Disabled")) {
            g_state.smbus_freeze = v;
        }
        break;
    }
    case V_CEC_CLOCK:
    case V_ONE_TOUCH:
        toggle_cec(item);
        break;
    case V_STANDBY:
        for (int i = 0; i < 3; i++) items[i] = kCecStandbyNames[i];
        open_value_list(PICK_ID_CEC_STANDBY, "TV AT CONSOLE POWER-OFF:", items, 3,
                        g_state.cec_standby_mode, item);
        break;
    case V_SPD:
        open_spd_list();
        break;
    case V_ADVANCED:
        s_set_adv = true;
        s_adv_sel = 0;
        break;
    case V_ADV_BACK:
        s_set_adv = false;
        break;
    }
}

static void input_tab_video(const input_state_t *in, int sel)
{
    input_settings_item(in, kSetMain[sel]);
}

// The Advanced page moves its own cursor; (B) goes back to the main page.
static void input_settings_adv(const input_state_t *in)
{
    if (in->b || in->back) {
        s_set_adv = false;
        return;
    }
    if (in->up && s_adv_sel > 0) s_adv_sel--;
    if (in->down && s_adv_sel < SET_ADV_COUNT - 1) s_adv_sel++;
    input_settings_item(in, kSetAdv[s_adv_sel]);
}

static void leave_tab_video(void)
{
    s_set_adv = false;
}

static const char *footer_tab_video(void)
{
    return s_set_adv ? "(A): Change | (B): Back | (X): Save Flash | (Y): Reload | Hold (BACK): Exit"
                     : "(A): Change | (X): Save Flash | (Y): Reload | Hold (BACK): Exit";
}

// The live raster block: Tab 3 shows the colour space on the wire, Tab 5 everything.
static void read_video_info(void)
{
    g_state.vi_ok = smbus_read_video_info(&g_state.vi);
}

// ---------------------------------------------------------------- Tab 2: Picture Alignment
//
// D-pad picks an item until "Adjust Position" is entered; then it moves the picture until (A)
// or (B). The firmware's offsets are its DE-generator delays, which move the capture window
// over the source -- the picture moves the other way -- so the D-pad and the readout use the
// picture's direction and negate the stored values.

enum { POS_MODE, POS_ADJUST, POS_RESET, POS_ITEMS };
static bool s_pos_adjust = false;
static int s_launch_mode;   // the video mode the app opened in; leaving Tabs 2 and 3 returns to it

static void render_tab_position(void)
{
    const video_mode_info_t *curMode = VideoMode_Get(VideoMode_GetCurrentIndex());
    const int sel = g_state.selected_item[TAB_POSITION];

    Terminal_PrintRow(5, COLOR_PHOSPHOR_DIM, "PICTURE POSITION & CENTERING CALIBRATION:");
    // Offsets are stored per timing profile; the board picks the profile from the console
    // mode it sees, reported here as the mode index (0xFF = standalone, no console packet).
    Terminal_PrintRow(6, COLOR_PHOSPHOR_NORMAL, "  Profile 0x%02X | Picture: %+d px right, %+d lines down",
                      g_state.vi.console_mode_index, -g_state.pos_h, -g_state.pos_v);

    Terminal_PrintItem(LIST_FIRST_ROW + POS_MODE, sel == POS_MODE, "Video Mode : %s",
                       curMode ? curMode->name : "Default");
    if (s_pos_adjust) {
        Terminal_PrintRow(LIST_FIRST_ROW + POS_ADJUST, COLOR_PHOSPHOR_AMBER, "> Adjust Position  [MOVING]");
    } else {
        Terminal_PrintItem(LIST_FIRST_ROW + POS_ADJUST, sel == POS_ADJUST, "Adjust Position");
    }
    Terminal_PrintItem(LIST_FIRST_ROW + POS_RESET, sel == POS_RESET, "Reset Position");

    // Below the reticle's centre box (screen rows 8-16).
    const int warn_row = Terminal_FooterTopRow() - 3;
    Terminal_PrintRow(warn_row, COLOR_PHOSPHOR_AMBER,
                      "Moving the picture far from centre can make the TV distort or lose it.");
    Terminal_PrintRow(warn_row + 1, COLOR_PHOSPHOR_AMBER,
                      "(Y) re-centres it at any time, even with nothing on screen.");

    // Hardware-aligned reticle centered at physical screen center
    Terminal_DrawAlignmentReticle(g_state.pos_h, g_state.pos_v);
}

static void reset_position(void)
{
    if (smbus_set_position_h(0)) g_state.pos_h = 0;
    if (smbus_set_position_v(0)) g_state.pos_v = 0;
    setting_written(g_state.pos_h == 0 && g_state.pos_v == 0, "Position reset to default (0, 0)");
}

static void open_video_mode_list(int row, int indent)
{
    static const char *items[PICK_MAX_ITEMS];
    const int n = VideoMode_GetCount();
    for (int i = 0; i < n && i < PICK_MAX_ITEMS; i++) items[i] = VideoMode_Get(i)->name;
    Picklist_Open(PICK_ID_VIDEO_MODE, "VIDEO MODE:", items, n, VideoMode_GetCurrentIndex(), row, indent);
}

static void input_tab_position(const input_state_t *in, int sel)
{
    if (in->y) {
        reset_position();   // works blind: the way back if the TV has lost the picture
        return;
    }
    if (!in->a) return;
    switch (sel) {
    case POS_MODE:   open_video_mode_list(LIST_FIRST_ROW + POS_MODE + 1, value_indent("Video Mode ")); break;
    case POS_ADJUST: s_pos_adjust = true; break;
    case POS_RESET:  reset_position(); break;
    }
}

// Moving the picture: the D-pad and triggers move it until (A) or (B).
static void input_position_adjust(const input_state_t *in)
{
    if (in->a || in->b || in->back) {
        s_pos_adjust = false;
        return;
    }
    if (in->y) {
        reset_position();
        return;
    }
    // Picture direction; the stored offsets are the negatives (see the comment above).
    int right = 0, down = 0;
    if (in->left) right -= 1;
    if (in->right) right += 1;
    if (in->up) down -= 1;
    if (in->down) down += 1;
    if (in->l_trigger) right -= 10;
    if (in->r_trigger) right += 10;

    if (right != 0) {
        int new_h = g_state.pos_h - right;
        if (new_h < -127) new_h = -127;
        if (new_h > 127) new_h = 127;
        if (setting_written(smbus_set_position_h((int8_t)new_h), "Live horizontal position updated")) {
            g_state.pos_h = (int8_t)new_h;
        }
    }
    if (down != 0) {
        int new_v = g_state.pos_v - down;
        if (new_v < -127) new_v = -127;
        if (new_v > 127) new_v = 127;
        if (setting_written(smbus_set_position_v((int8_t)new_v), "Live vertical position updated")) {
            g_state.pos_v = (int8_t)new_v;
        }
    }
}

static void switch_video_mode(int idx)
{
    const video_mode_info_t *m = VideoMode_Get(idx);
    if (!m || idx == VideoMode_GetCurrentIndex()) {
        return;
    }
    if (Terminal_SetVideoMode(m->width, m->height, m->d3d_flags, m->refresh_rate)) {
        VideoMode_SetCurrentIndex(idx);
        Sleep(200); // Allow ADV7511 line detector & clock to settle
        g_state.vi_ok = smbus_read_video_info(&g_state.vi);
        if (g_state.vi_ok) {
            g_state.pos_h = g_state.vi.pos_h;
            g_state.pos_v = g_state.vi.pos_v;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Switched mode: %s", m->name);
        set_status(msg);
    } else {
        set_alert("ERROR: Failed to switch video mode (D3D reset error)");
    }
}

// Back to the launch mode when leaving Tabs 2 and 3, which can change it.
static void restore_launch_mode(void)
{
    const video_mode_info_t *m = VideoMode_Get(s_launch_mode);
    if (m && VideoMode_GetCurrentIndex() != s_launch_mode &&
        Terminal_SetVideoMode(m->width, m->height, m->d3d_flags, m->refresh_rate)) {
        VideoMode_SetCurrentIndex(s_launch_mode);
    }
}

static const char *footer_tab_position(void)
{
    if (s_pos_adjust) {
        return "D-PAD: Move 1 | TRIGGERS: Move 10 (H) | (Y): Centre | (X): Save Flash | (A)/(B): Done";
    }
    return "(A): Select | (Y): Re-centre | (X): Save Flash | Hold (BACK): Exit";
}

// ---------------------------------------------------------------- Tab 3: Picture
//
// The colour space, the board's picture controls (READ_PICTURE in doc/SMBUS.md)
// and a test pattern to judge them on. The board keeps three profiles -- SD, HD and
// non-standard -- and picks the one in use from the signal; the controls edit one of them, the
// one in use unless another is picked on the Profile line. A control is entered with (A); then
// Left/Right change it live until (A) or (B), the way Tab 2 moves the picture. White balance
// (per-colour gain and offset) is a page of its own, opened from the list. The pattern also
// opens on the whole screen.

// The controls, whichever page shows them.
enum {
    C_WHITE, C_BLACK, C_CONTRAST, C_BRIGHT, C_SAT, C_HUE,
    C_GAIN_R, C_GAIN_G, C_GAIN_B, C_BIAS_R, C_BIAS_G, C_BIAS_B, C_COUNT
};
static const char *const kCtlNames[C_COUNT] = {
    "White Clip", "Black Clip", "Contrast", "Brightness", "Saturation", "Hue",
    "Red Gain", "Green Gain", "Blue Gain", "Red Offset", "Green Offset", "Blue Offset",
};
// The same three rows act on Cr, Y and Cb in YCbCr Adjustable.
static const char *const kCtlNamesYcc[6] = {
    "Cr Gain", "Y Gain", "Cb Gain", "Cr Offset", "Y Offset", "Cb Offset",
};

// The main page.
enum {
    P_MODE, P_COLOR, P_PROFILE, P_WHITE, P_BLACK, P_CONTRAST, P_BRIGHT, P_SAT, P_HUE,
    P_FULL, P_DEFAULTS, P_WB, P_COPY, P_ITEMS
};
static const char *const kPicLabels[P_ITEMS] = {
    "Video Mode", "Colour Space", "Profile", "White Clip", "Black Clip", "Contrast", "Brightness",
    "Saturation", "Hue", "Full-Screen Pattern", "Reset Picture to Defaults",
    "White Balance (R/G/B)...", "Copy Profile to All",
};
// What the selected item does, under the list (two lines, each narrower than the column).
static const char *const kPicHints[P_ITEMS][2] = {
    { "The mode this app runs in on Tabs 2 and 3. A TV may",
      "treat modes differently: check the ones you use." },
    { "Full (0-255) or Limited (16-235) black and white levels.",
      "The passthroughs skip the controls; Adjustable doesn't." },
    { "SD, HD and Non-standard signals each keep their own",
      "settings. (A) picks the one the controls below edit." },
    { "The brightest level sent. Lower it if the TV loses the",
      "picture on bright white. Default 250." },
    { "The darkest level sent. Raise it only if the TV acts up",
      "on black. Default 0." },
    { "Gain on the whites, black staying put. Default 100 %.",
      "Too high and the near-white steps merge." },
    { "Lifts or lowers every level. Default 0. Step 0 should be",
      "black with 2-4 just visible." },
    { "Colour strength: 0 % is black and white. Default 100 %.",
      "Judge it on the colour bars, or better, in a game." },
    { "Turns every colour: + moves red toward yellow, - toward",
      "magenta. Default 0. Judge it on skin tones." },
    { "The whole-screen pattern: Up/Down picks a control,",
      "Left/Right change it, (B) comes back." },
    { "This profile back to neutral: white clip 250, the rest",
      "neutral, white balance too. Live until (X) saves." },
    { "Per-colour gain and offset for this profile (R/G/B, or",
      "Cr/Y/Cb in YCbCr Adjustable): takes out a colour cast." },
    { "Copies this profile over the other two, so every signal",
      "gets the same picture. Asks for a second (A)." },
};

// The white balance page.
enum { W_GAIN_R, W_GAIN_G, W_GAIN_B, W_BIAS_R, W_BIAS_G, W_BIAS_B, W_RESET, W_BACK, W_ITEMS };
static const char *const kWbActions[2] = { "Reset White Balance", "Back to Picture" };
static const char *const kWbHints[W_ITEMS][2] = {
    { "Red's share of the whites, black staying put. Lower it",
      "if whites look pink or orange. Default 100 %." },
    { "Green's share of the whites, black staying put. Lower it",
      "if whites look green. Default 100 %." },
    { "Blue's share of the whites, black staying put. Lower it",
      "if whites look blue. Default 100 %." },
    { "Red added to every level, dark ones included. Lower it",
      "if black and dark greys look red. Default 0." },
    { "Green added to every level, dark ones included. Lower",
      "it if black and dark greys look green. Default 0." },
    { "Blue added to every level, dark ones included. Lower it",
      "if black and dark greys look blue. Default 0." },
    { "Gains back to 100 %, offsets to 0, in this profile only.",
      "Live until (X) saves." },
    { "Back to the picture controls. (B) does the same.", "" },
};
// The six controls' hints when the rows are Cr, Y, Cb.
static const char *const kWbHintsYcc[6][2] = {
    { "Red-difference strength: saturation along the red-cyan",
      "axis only. Default 100 %." },
    { "Gain on brightness, black staying put: works like",
      "contrast. Default 100 %." },
    { "Blue-difference strength: saturation along the blue-",
      "yellow axis only. Default 100 %." },
    { "Shifts every level toward red (+) or cyan (-).",
      "Default 0." },
    { "Lifts or lowers brightness, like Brightness.",
      "Default 0." },
    { "Shifts every level toward blue (+) or yellow (-).",
      "Default 0." },
};

static const char *const kProfileNames[XHD_PIC_PROFILES] = { "SD", "HD", "Non-standard" };
static const char *const kProfileItems[XHD_PIC_PROFILES] = {
    "SD -- 480p / 576p / 480i on the 27 MHz clock",
    "HD -- 720p and 1080i",
    "Non-standard -- SD on any other clock",
};

#define PIC_FIRST_ROW 6             // the list starts a row higher than the other tabs': 13 items
#define PIC_VALUE_X  220            // value column, px from the left margin
#define PIC_BAR_X    340            // slider bar
#define PIC_BAR_W    190
#define PIC_PANEL_X  660.0f         // the pattern, in screen px
#define PIC_PANEL_W  540.0f

static bool s_pic_adjust = false;   // Left/Right change the selected control until (A)/(B)
static bool s_pic_wb = false;       // the white balance page is showing
static int  s_wb_sel = 0;           // its cursor
static bool s_pic_full = false;     // the pattern on the whole screen
static int  s_pic_full_ctl;         // which control Left/Right change there (kFullCtl)
static int  s_pic_edit = 0;         // the profile the controls edit
static bool s_pic_edit_follows = true;  // ...which follows the one in use until another is picked

// Hue as last set here, per profile: at low saturation the Q12 pair can't hold every degree,
// and at 0 % it holds none, so the angle is kept as long as the board's pair still matches it.
static int  s_hue_deg[XHD_PIC_PROFILES];
static bool s_hue_known[XHD_PIC_PROFILES];

#define FULL_COLOR (-1)             // the colour space, in kFullCtl
static const int kFullCtl[] = {
    FULL_COLOR, C_WHITE, C_BLACK, C_CONTRAST, C_BRIGHT, C_SAT, C_HUE,
    C_GAIN_R, C_GAIN_G, C_GAIN_B, C_BIAS_R, C_BIAS_G, C_BIAS_B,
};
#define FULL_CTL_COUNT ((int)(sizeof(kFullCtl) / sizeof(kFullCtl[0])))

#define PIC_PI 3.14159265358979323846

static int pic_row_y(int row)
{
    return Terminal_GetTopMargin() + row * Terminal_GetLineHeight();
}

// A main-page or white-balance item's control, or -1 for the other items.
static int pic_item_ctl(int item)
{
    return item >= P_WHITE && item <= P_HUE ? C_WHITE + (item - P_WHITE) : -1;
}

static int wb_item_ctl(int item)
{
    return item >= W_GAIN_R && item <= W_BIAS_B ? C_GAIN_R + item : -1;
}

// The white balance page's rows: a gap between the gains, the offsets and the actions.
static int wb_row(int item)
{
    return PIC_FIRST_ROW + item + (item >= W_BIAS_R ? 1 : 0) + (item >= W_RESET ? 1 : 0);
}

// The edited profile follows the one in use until the Profile line picks another.
static void pic_sync_edit(void)
{
    if (g_state.pic_ok && s_pic_edit_follows) s_pic_edit = g_state.pic.live;
}

// What the board sends: the wire readout when a mode is applied, else the setting.
static uint8_t wire_color_space(void)
{
    if (g_state.vi_ok && (g_state.vi.flags & XHD_VI_FLAG_APPLIED)) return g_state.vi.color_space;
    return g_state.color_space;
}

static bool is_passthrough(uint8_t cs)
{
    return cs == XHD_COLOR_SPACE_YCBCR_PASSTHROUGH || cs == XHD_COLOR_SPACE_RGB_PASSTHROUGH;
}

// The gain and bias rows are Cr, Y, Cb when YCbCr Adjustable actually goes out (a sink without
// YCbCr 4:4:4 gets RGB Full instead, and the wire readout says so).
static bool wb_rows_ycc(void)
{
    return wire_color_space() == XHD_COLOR_SPACE_YCBCR_CSC;
}

static const char *ctl_name(int ctl)
{
    return ctl >= C_GAIN_R && wb_rows_ycc() ? kCtlNamesYcc[ctl - C_GAIN_R] : kCtlNames[ctl];
}

// Why the picture controls can't act now, or NULL when they can.
static const char *picture_blocked(void)
{
    if (!g_state.xhd_present) return "No board answering";
    if (!g_state.pic_ok) return "Picture controls need newer firmware: update on Tab 8";
    if (is_passthrough(wire_color_space())) return "Passthrough colour space: the controls don't apply";
    return NULL;
}

static void pic_range(int ctl, int *min, int *max, int *def)
{
    switch (ctl) {
    case C_WHITE:    *min = XHD_PIC_WHITE_MIN;    *max = XHD_PIC_WHITE_MAX;    *def = XHD_PIC_WHITE_DEFAULT; break;
    case C_BLACK:    *min = 0;                    *max = XHD_PIC_BLACK_MAX;    *def = 0; break;
    case C_CONTRAST: *min = XHD_PIC_CONTRAST_MIN; *max = XHD_PIC_CONTRAST_MAX; *def = 100; break;
    case C_BRIGHT:   *min = -XHD_PIC_BRIGHT_MAX;  *max = XHD_PIC_BRIGHT_MAX;   *def = 0; break;
    case C_SAT:      *min = 0;                    *max = XHD_PIC_SAT_MAX;      *def = 100; break;
    case C_HUE:      *min = -XHD_PIC_HUE_MAX;     *max = XHD_PIC_HUE_MAX;      *def = 0; break;
    case C_GAIN_R: case C_GAIN_G: case C_GAIN_B:
        *min = XHD_PIC_GAIN_MIN; *max = XHD_PIC_GAIN_MAX; *def = XHD_PIC_GAIN_DEFAULT; break;
    default:
        *min = -XHD_PIC_BIAS_MAX; *max = XHD_PIC_BIAS_MAX; *def = 0; break;
    }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The Q12 pair for a saturation (percent) and a hue (degrees).
static void pic_chroma(int sat, int hue, int16_t *c, int16_t *s)
{
    const double r = sat * (double)XHD_PIC_Q12_ONE / 100.0;
    const double a = hue * (PIC_PI / 180.0);
    *c = (int16_t)floor(r * cos(a) + 0.5);
    *s = (int16_t)floor(r * sin(a) + 0.5);
}

static int pic_sat(const XHD_PICTURE *p)
{
    const double r = hypot((double)p->chroma_cos, (double)p->chroma_sin);
    return clampi((int)floor(r * 100.0 / XHD_PIC_Q12_ONE + 0.5), 0, XHD_PIC_SAT_MAX);
}

static int pic_hue(const XHD_PICTURE *p, int prof)
{
    if (s_hue_known[prof]) {
        int16_t c, s;
        pic_chroma(pic_sat(p), s_hue_deg[prof], &c, &s);
        if (c == p->chroma_cos && s == p->chroma_sin) return s_hue_deg[prof];
    }
    if (p->chroma_cos == 0 && p->chroma_sin == 0) return 0;
    const double deg = atan2((double)p->chroma_sin, (double)p->chroma_cos) * (180.0 / PIC_PI);
    return clampi((int)floor(deg + 0.5), -XHD_PIC_HUE_MAX, XHD_PIC_HUE_MAX);
}

static int pic_get(const XHD_PICTURE *p, int prof, int ctl)
{
    switch (ctl) {
    case C_WHITE:    return p->white_clip;
    case C_BLACK:    return p->black_clip;
    case C_CONTRAST: return p->contrast;
    case C_BRIGHT:   return p->brightness;
    case C_SAT:      return pic_sat(p);
    case C_HUE:      return pic_hue(p, prof);
    case C_GAIN_R: case C_GAIN_G: case C_GAIN_B: return p->gain[ctl - C_GAIN_R];
    default:         return p->bias[ctl - C_BIAS_R];
    }
}

static void pic_set(XHD_PICTURE *p, int prof, int ctl, int v)
{
    switch (ctl) {
    case C_WHITE:    p->white_clip = (uint8_t)v; break;
    case C_BLACK:    p->black_clip = (uint8_t)v; break;
    case C_CONTRAST: p->contrast = (uint8_t)v; break;
    case C_BRIGHT:   p->brightness = (int8_t)v; break;
    case C_SAT:
    case C_HUE: {
        // Both live in the one Q12 pair: change one, keep the other.
        const int sat = ctl == C_SAT ? v : pic_sat(p);
        const int hue = ctl == C_HUE ? v : pic_hue(p, prof);
        pic_chroma(sat, hue, &p->chroma_cos, &p->chroma_sin);
        s_hue_deg[prof] = hue;
        s_hue_known[prof] = true;
        break;
    }
    case C_GAIN_R: case C_GAIN_G: case C_GAIN_B: p->gain[ctl - C_GAIN_R] = (uint8_t)v; break;
    default:         p->bias[ctl - C_BIAS_R] = (int8_t)v; break;
    }
}

static void pic_value_str(int ctl, int v, char *buf, size_t n)
{
    switch (ctl) {
    case C_CONTRAST:
    case C_SAT:
    case C_GAIN_R: case C_GAIN_G: case C_GAIN_B:
        snprintf(buf, n, "%d %%", v); break;
    case C_HUE:    snprintf(buf, n, "%+d deg", v); break;
    case C_BRIGHT:
    case C_BIAS_R: case C_BIAS_G: case C_BIAS_B:
        snprintf(buf, n, "%+d", v); break;
    default:       snprintf(buf, n, "%d", v); break;
    }
}

// Writes one profile and takes the board's read-back as the new state. `what` is the status
// message's start; it says whether the change shows now.
static bool pic_write_profile(int prof, const XHD_PICTURE *p, const char *what)
{
    XHD_PICTURE_SET now;
    const bool ok = smbus_write_picture((uint8_t)prof, p, &now);
    char msg[96];
    if (prof == g_state.pic.live) {
        snprintf(msg, sizeof(msg), "%s (%s, live)", what, kProfileNames[prof]);
    } else {
        snprintf(msg, sizeof(msg), "%s (%s profile: shows when %s is in use)", what,
                 kProfileNames[prof], kProfileNames[prof]);
    }
    if (setting_written(ok, msg)) {
        g_state.pic = now;
        pic_sync_edit();
    }
    return ok;
}

static void pic_change(int ctl, int delta)
{
    const char *why = picture_blocked();
    if (why) {
        set_status(why);
        return;
    }
    const int prof = s_pic_edit;
    int min, max, def;
    pic_range(ctl, &min, &max, &def);
    const int old = pic_get(&g_state.pic.prof[prof], prof, ctl);
    const int v = clampi(old + delta, min, max);
    if (v == old) return;
    XHD_PICTURE p = g_state.pic.prof[prof];
    pic_set(&p, prof, ctl, v);
    char val[16], what[48];
    pic_value_str(ctl, pic_get(&p, prof, ctl), val, sizeof(val));
    snprintf(what, sizeof(what), "%s: %s", ctl_name(ctl), val);
    pic_write_profile(prof, &p, what);
}

// Neutral settings for the edited profile: all of them, or only the white balance.
static void pic_defaults(bool wb_only)
{
    if (!g_state.xhd_present || !g_state.pic_ok) {
        set_status(picture_blocked());
        return;
    }
    const int prof = s_pic_edit;
    static const XHD_PICTURE neutral = {
        XHD_PIC_WHITE_DEFAULT, 0, 100, 0, XHD_PIC_Q12_ONE, 0,
        { XHD_PIC_GAIN_DEFAULT, XHD_PIC_GAIN_DEFAULT, XHD_PIC_GAIN_DEFAULT }, { 0, 0, 0 },
    };
    XHD_PICTURE p = neutral;
    if (wb_only) {
        p = g_state.pic.prof[prof];
        memcpy(p.gain, neutral.gain, sizeof(p.gain));
        memcpy(p.bias, neutral.bias, sizeof(p.bias));
    } else {
        s_hue_known[prof] = false;
    }
    pic_write_profile(prof, &p, wb_only ? "White balance back to defaults" : "Picture back to defaults");
}

// The edited profile over the other two, on a second (A).
static void pic_copy_all(void)
{
    if (!g_state.xhd_present || !g_state.pic_ok) {
        set_status(picture_blocked());
        return;
    }
    const int src = s_pic_edit;
    if (!confirm_armed(&s_confirm, CONF_PIC_COPY)) {
        confirm_arm(&s_confirm, CONF_PIC_COPY);
        char msg[96];
        snprintf(msg, sizeof(msg), "Press (A) again to CONFIRM: copy the %s profile over the other two",
                 kProfileNames[src]);
        set_status(msg);
        return;
    }
    confirm_clear(&s_confirm);
    const XHD_PICTURE p = g_state.pic.prof[src];
    bool ok = true, any = false;
    XHD_PICTURE_SET now;
    for (int i = 0; i < XHD_PIC_PROFILES && ok; i++) {
        if (i == src) continue;
        ok = smbus_write_picture((uint8_t)i, &p, &now);
        if (ok) {
            any = true;
            g_state.pic = now;
            s_hue_deg[i] = s_hue_deg[src];
            s_hue_known[i] = s_hue_known[src];
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%s profile copied to all three (live)", kProfileNames[src]);
    setting_written(ok, msg);
    if (any) s_unsaved = true;   // a later write failed, but the first one changed the board
}

// The board's profile in use, re-read with the raster block every second: a mode change can
// move it (about 300 ms after the new clock settles).
static void enter_tab_picture(void)
{
    read_video_info();
    uint8_t live;
    if (g_state.pic_ok && smbus_read_picture_live(&live) && live != g_state.pic.live) {
        g_state.pic.live = live;
        pic_sync_edit();
    }
}

static void leave_tab_picture(void)
{
    s_pic_wb = false;
    s_pic_adjust = false;
}

// The wire readout follows a colour change within half a second rather than the next 1 s poll.
static void reread_wire_soon(void)
{
    g_state.last_poll_tick = GetTickCount() - 500;
}

// Tabs 1 and 3. The board reads the value back; firmware from before YCbCr Adjustable keeps
// its setting when sent that one.
static void set_color_space(uint8_t cs)
{
    if (smbus_set_color_space(cs)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Colour Space: %s (live)", XhdName_ColorSpaceShort(cs));
        setting_written(true, msg);
        g_state.color_space = cs;
        reread_wire_soon();
    } else if (cs == XHD_COLOR_SPACE_YCBCR_CSC && g_state.xhd_present) {
        set_alert("YCbCr Adjustable needs newer firmware: update on Tab 8");
    } else {
        setting_written(false, NULL);
    }
}

static void pic_color_step(int dir)
{
    set_color_space((uint8_t)((g_state.color_space + XHD_COLOR_SPACE_COUNT + dir) % XHD_COLOR_SPACE_COUNT));
}

static void open_profile_list(void)
{
    if (!g_state.pic_ok) {
        set_status(picture_blocked());
        return;
    }
    Picklist_Open(PICK_ID_PIC_PROFILE, "PROFILE TO EDIT:", kProfileItems, XHD_PIC_PROFILES, s_pic_edit,
                  PIC_FIRST_ROW + P_PROFILE + 1, PIC_VALUE_X);
}

static void pic_pick_profile(int chosen)
{
    if (chosen < 0 || chosen >= XHD_PIC_PROFILES) return;
    s_pic_edit = chosen;
    s_pic_edit_follows = chosen == g_state.pic.live;
    char msg[96];
    if (s_pic_edit_follows) {
        snprintf(msg, sizeof(msg), "Editing the %s profile, the one in use now", kProfileNames[chosen]);
    } else {
        snprintf(msg, sizeof(msg), "Editing the %s profile: changes show when %s is in use",
                 kProfileNames[chosen], kProfileNames[chosen]);
    }
    set_status(msg);
}

// Left/Right and the triggers as a step: +-1 and +-10.
static int pic_delta(const input_state_t *in)
{
    return (in->right ? 1 : 0) - (in->left ? 1 : 0) + (in->r_trigger ? 10 : 0) - (in->l_trigger ? 10 : 0);
}

static void draw_slider(int row, int ctl, DWORD color)
{
    int min, max, def;
    pic_range(ctl, &min, &max, &def);
    const int v = pic_get(&g_state.pic.prof[s_pic_edit], s_pic_edit, ctl);
    const float x = (float)(Terminal_GetLeftMargin() + PIC_BAR_X);
    const float y = (float)(pic_row_y(row) + 9);
    const float h = 10.0f;
    const float span = (float)(max - min);
    Terminal_DrawRect(x, y, (float)PIC_BAR_W, h, COLOR_PHOSPHOR_DIM);
    Terminal_DrawFilledRect(x + 1.0f, y + 1.0f, ((float)PIC_BAR_W - 2.0f) * (float)(v - min) / span, h - 2.0f, color);
    const float xd = x + (float)PIC_BAR_W * (float)(def - min) / span;   // the default, as a tick
    Terminal_DrawLine(xd, y - 4.0f, xd, y + h + 4.0f, COLOR_PHOSPHOR_ACCENT);
}

// A slider row: the label, the value (in < > while changing) and the bar.
static void draw_control_row(int row, const char *label, int ctl, bool here, const char *why, DWORD bar_color)
{
    DWORD c = here ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL;
    if (why && !here) c = COLOR_PHOSPHOR_DIM;
    if (here && s_pic_adjust) c = COLOR_PHOSPHOR_AMBER;
    Terminal_PrintRow(row, c, "%s%s", here ? "> " : "  ", label);
    if (!g_state.pic_ok) {
        Terminal_PrintRowIndent(row, PIC_VALUE_X, c, "--");
        return;
    }
    char v[16];
    pic_value_str(ctl, pic_get(&g_state.pic.prof[s_pic_edit], s_pic_edit, ctl), v, sizeof(v));
    Terminal_PrintRowIndent(row, PIC_VALUE_X, c, here && s_pic_adjust ? "< %s >" : "%s", v);
    draw_slider(row, ctl, why ? COLOR_PHOSPHOR_DARK : (here ? COLOR_PHOSPHOR_BRIGHT : bar_color));
}

// The profile line's value: the one edited, and which one is in use when that's another.
static void profile_value_str(char *buf, size_t n)
{
    if (!g_state.pic_ok) {
        snprintf(buf, n, "--");
    } else if (s_pic_edit == g_state.pic.live) {
        snprintf(buf, n, "%s (in use)", kProfileNames[s_pic_edit]);
    } else {
        snprintf(buf, n, "%s (in use: %s)", kProfileNames[s_pic_edit], kProfileNames[g_state.pic.live]);
    }
}

static void draw_pic_hints(int row, const char *const hint[2])
{
    Terminal_PrintRow(row, COLOR_PHOSPHOR_DIM, "%s", hint[0]);
    Terminal_PrintRow(row + 1, COLOR_PHOSPHOR_DIM, "%s", hint[1]);
}

static void draw_pic_pattern(void)
{
    const float top = (float)pic_row_y(5);
    Pattern_Draw(PIC_PANEL_X, top, PIC_PANEL_W, (float)pic_row_y(Terminal_FooterTopRow()) - top - 6.0f);
}

static void render_picture_wb(void)
{
    const char *why = picture_blocked();
    if (why) {
        Terminal_PrintRow(5, COLOR_PHOSPHOR_AMBER, "%s", why);
    } else {
        char prof[48];
        profile_value_str(prof, sizeof(prof));
        Terminal_PrintRow(5, COLOR_PHOSPHOR_DIM, "%s -- profile %s:",
                          wb_rows_ycc() ? "WHITE BALANCE (Cr / Y / Cb)" : "WHITE BALANCE", prof);
    }
    // Each colour's bar in its own colour, so the rows read at a glance.
    static const DWORD kRowColor[3] = { 0xFFE05050, 0xFF50E050, 0xFF6080FF };
    for (int i = 0; i < W_ITEMS; i++) {
        const int ctl = wb_item_ctl(i);
        const bool here = s_wb_sel == i;
        if (ctl >= 0) {
            draw_control_row(wb_row(i), ctl_name(ctl), ctl, here, why, kRowColor[(ctl - C_GAIN_R) % 3]);
        } else {
            Terminal_PrintItem(wb_row(i), here, "%s", kWbActions[i - W_RESET]);
        }
    }
    draw_pic_hints(wb_row(W_ITEMS - 1) + 2,
                   s_wb_sel <= W_BIAS_B && wb_rows_ycc() ? kWbHintsYcc[s_wb_sel] : kWbHints[s_wb_sel]);
    draw_pic_pattern();
}

static void render_tab_picture(void)
{
    if (s_pic_wb) {
        render_picture_wb();
        return;
    }
    const int sel = g_state.selected_item[TAB_PICTURE];
    const char *why = picture_blocked();

    if (why) {
        Terminal_PrintRow(5, COLOR_PHOSPHOR_AMBER, "%s", why);
    } else {
        Terminal_PrintRow(5, COLOR_PHOSPHOR_DIM, "PICTURE (APPLIED LIVE) -- sending %s:",
                          XhdName_ColorSpaceShort(wire_color_space()));
    }

    for (int i = 0; i < P_ITEMS; i++) {
        const int row = PIC_FIRST_ROW + i;
        const bool here = sel == i;
        const int ctl = pic_item_ctl(i);
        if (ctl >= 0) {
            draw_control_row(row, kPicLabels[i], ctl, here, why, COLOR_PHOSPHOR_NORMAL);
            continue;
        }
        const bool needs_pic = i == P_PROFILE || i >= P_DEFAULTS;
        DWORD c = here ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL;
        if (needs_pic && !g_state.pic_ok && !here) c = COLOR_PHOSPHOR_DIM;
        const char *label = i == P_WB && wb_rows_ycc() ? "White Balance (Cr/Y/Cb)..." : kPicLabels[i];
        Terminal_PrintRow(row, c, "%s%s", here ? "> " : "  ", label);

        char val[48] = "";
        if (i == P_MODE) {
            const video_mode_info_t *m = VideoMode_Get(VideoMode_GetCurrentIndex());
            snprintf(val, sizeof(val), "%s", m ? m->name : "Default");
        } else if (i == P_COLOR) {
            snprintf(val, sizeof(val), "%s", XhdName_ColorSpaceShort(g_state.color_space));
        } else if (i == P_PROFILE) {
            profile_value_str(val, sizeof(val));
            if (g_state.pic_ok && s_pic_edit != g_state.pic.live) c = COLOR_PHOSPHOR_AMBER;
        }
        if (val[0]) Terminal_PrintRowIndent(row, PIC_VALUE_X, c, "%s", val);
    }

    if (sel == P_COPY && confirm_armed(&s_confirm, CONF_PIC_COPY)) {
        Terminal_PrintRow(PIC_FIRST_ROW + P_ITEMS, COLOR_PHOSPHOR_AMBER, "  -> (A) again to copy, (B) to cancel");
        Terminal_PrintRow(PIC_FIRST_ROW + P_ITEMS + 1, COLOR_PHOSPHOR_DIM, "%s", kPicHints[sel][1]);
    } else {
        draw_pic_hints(PIC_FIRST_ROW + P_ITEMS, kPicHints[sel]);
    }
    draw_pic_pattern();
}

static void input_tab_picture(const input_state_t *in, int sel)
{
    static const char *items[PICK_MAX_ITEMS];
    if ((in->b || in->back) && confirm_any(&s_confirm)) {
        cancel_confirms();
        set_status("Action cancelled");
        return;
    }
    if (!in->a) return;
    switch (sel) {
    case P_MODE:
        open_video_mode_list(PIC_FIRST_ROW + P_MODE + 1, PIC_VALUE_X);
        break;
    case P_COLOR:
        for (int i = 0; i < XHD_COLOR_SPACE_COUNT; i++) items[i] = XhdName_ColorSpace((uint8_t)i);
        Picklist_Open(PICK_ID_COLOR_SPACE, "COLOUR SPACE:", items, XHD_COLOR_SPACE_COUNT, g_state.color_space,
                      PIC_FIRST_ROW + P_COLOR + 1, PIC_VALUE_X);
        break;
    case P_PROFILE:
        open_profile_list();
        break;
    case P_FULL:
        s_pic_full = true;
        s_pic_full_ctl = 0;
        break;
    case P_DEFAULTS:
        pic_defaults(false);
        break;
    case P_WB:
        if (g_state.pic_ok) {
            s_pic_wb = true;
            s_wb_sel = 0;
        } else {
            set_status(picture_blocked());
        }
        break;
    case P_COPY:
        pic_copy_all();
        break;
    default: {
        const char *why = picture_blocked();
        if (why) set_status(why); else s_pic_adjust = true;
        break;
    }
    }
}

// The white balance page: (A) on a colour starts changing it, (B) goes back to the main page.
static void input_picture_wb(const input_state_t *in)
{
    if (in->b || in->back) {
        s_pic_wb = false;
        return;
    }
    if (in->up && s_wb_sel > 0) s_wb_sel--;
    if (in->down && s_wb_sel < W_ITEMS - 1) s_wb_sel++;
    if (!in->a) return;
    switch (s_wb_sel) {
    case W_RESET: pic_defaults(true); break;
    case W_BACK:  s_pic_wb = false; break;
    default: {
        const char *why = picture_blocked();
        if (why) set_status(why); else s_pic_adjust = true;
        break;
    }
    }
}

// Adjusting: Left/Right and the triggers change the control, Up/Down move to the next one on
// the same page.
static void input_picture_adjust(const input_state_t *in)
{
    if (in->a || in->b || in->back) {
        s_pic_adjust = false;
        return;
    }
    int *sel;
    int first, last, ctl;
    if (s_pic_wb) {
        sel = &s_wb_sel;
        first = W_GAIN_R;
        last = W_BIAS_B;
    } else {
        sel = &g_state.selected_item[TAB_PICTURE];
        first = P_WHITE;
        last = P_HUE;
    }
    if (in->up && *sel > first) (*sel)--;
    if (in->down && *sel < last) (*sel)++;
    ctl = s_pic_wb ? wb_item_ctl(*sel) : pic_item_ctl(*sel);
    const int d = pic_delta(in);
    if (d && ctl >= 0) pic_change(ctl, d);
}

static void input_picture_full(const input_state_t *in)
{
    if (in->b || in->back) {
        s_pic_full = false;
        return;
    }
    if (in->up && s_pic_full_ctl > 0) s_pic_full_ctl--;
    if (in->down && s_pic_full_ctl < FULL_CTL_COUNT - 1) s_pic_full_ctl++;
    const int d = pic_delta(in);
    if (!d) return;
    const int ctl = kFullCtl[s_pic_full_ctl];
    if (ctl == FULL_COLOR) pic_color_step(d > 0 ? 1 : -1); else pic_change(ctl, d);
}

// The pattern on the whole screen, the control being changed above it and how to read it below.
static void render_picture_full(void)
{
    const float sw = (float)Terminal_GetWidth();
    Terminal_DrawFilledRect(0.0f, 0.0f, sw, (float)Terminal_GetHeight(), D3DCOLOR_XRGB(0, 0, 0));

    const int ctl = kFullCtl[s_pic_full_ctl];
    char val[48];
    if (ctl == FULL_COLOR) {
        snprintf(val, sizeof(val), "%s", XhdName_ColorSpaceShort(g_state.color_space));
    } else if (g_state.pic_ok) {
        pic_value_str(ctl, pic_get(&g_state.pic.prof[s_pic_edit], s_pic_edit, ctl), val, sizeof(val));
    } else {
        snprintf(val, sizeof(val), "--");
    }
    const char *why = ctl == FULL_COLOR ? NULL : picture_blocked();
    if (g_state.pic_ok) {
        Terminal_PrintRowCenter(0, COLOR_PHOSPHOR_DIM, "PICTURE TEST PATTERN -- %s PROFILE  (%d of %d)",
                                kProfileNames[s_pic_edit], s_pic_full_ctl + 1, FULL_CTL_COUNT);
    } else {
        Terminal_PrintRowCenter(0, COLOR_PHOSPHOR_DIM, "PICTURE TEST PATTERN  (%d of %d)", s_pic_full_ctl + 1, FULL_CTL_COUNT);
    }
    Terminal_PrintRowCenter(1, why ? COLOR_PHOSPHOR_DIM : COLOR_PHOSPHOR_BRIGHT, "<  %s: %s  >",
                            ctl == FULL_COLOR ? "Colour Space" : ctl_name(ctl), val);
    if (why) {
        Terminal_PrintRowCenter(2, COLOR_PHOSPHOR_AMBER, "%s", why);
    } else if (ctl != FULL_COLOR && s_pic_edit != g_state.pic.live) {
        Terminal_PrintRowCenter(2, COLOR_PHOSPHOR_AMBER, "The %s profile isn't in use now (%s is): changes won't show here",
                                kProfileNames[s_pic_edit], kProfileNames[g_state.pic.live]);
    } else if (g_state.vi_ok && (g_state.vi.flags & XHD_VI_FLAG_APPLIED)) {
        Terminal_PrintRowCenter(2, COLOR_PHOSPHOR_NORMAL, "Colour Space: %s   On wire: %s",
                                XhdName_ColorSpaceShort(g_state.color_space), XhdName_ColorSpaceShort(g_state.vi.color_space));
    } else {
        Terminal_PrintRowCenter(2, COLOR_PHOSPHOR_NORMAL, "Colour Space: %s   On wire: no mode applied",
                                XhdName_ColorSpaceShort(g_state.color_space));
    }

    const int bottom = Terminal_FooterTopRow() + 2;
    const float top = (float)pic_row_y(3) + 4.0f;
    Pattern_Draw(sw * 0.06f, top, sw * 0.88f, (float)pic_row_y(bottom - 4) - top - 4.0f);

    // A mismatch between the board's range and the TV's HDMI range setting shows one of two
    // ways; the board flags the range in the AVI InfoFrame, but some TVs ignore it.
    Terminal_PrintRow(bottom - 4, COLOR_PHOSPHOR_NORMAL,
                      "  * Right: 0 is deep black, 2-4 just visible above it, whites distinct up to the white clip.");
    Terminal_PrintRow(bottom - 3, COLOR_PHOSPHOR_AMBER,
                      "  * Steps up to ~16 all black and whites merged: the TV expects Limited. Try RGB Limited.");
    Terminal_PrintRow(bottom - 2, COLOR_PHOSPHOR_AMBER,
                      "  * Black looks grey and every step shows: the TV expects Full. Try RGB Full.");
    Terminal_DrawHLine(bottom - 1, '-', COLOR_PHOSPHOR_DARK);
    if (g_state.status_msg[0]) {
        Terminal_PrintRow(bottom, s_status_alert ? COLOR_PHOSPHOR_AMBER : COLOR_PHOSPHOR_NORMAL, "STATUS: %s", g_state.status_msg);
    } else {
        Terminal_PrintRow(bottom, COLOR_PHOSPHOR_BRIGHT,
                          "KEYS: UP/DOWN: Control | L/R: Change 1 | TRIGGERS: 10 | (X): Save Flash | (B): Back");
    }
}

static const char *footer_tab_picture(void)
{
    if (s_pic_adjust) {
        return "D-PAD L/R: Change 1 | TRIGGERS: Change 10 | UP/DOWN: Next Control | (X): Save Flash | (A)/(B): Done";
    }
    if (s_pic_wb) {
        return "(A): Change | (B): Back | (X): Save Flash | (Y): Reload | Hold (BACK): Exit";
    }
    return "(A): Change | (X): Save Flash | (Y): Reload | Hold (BACK): Exit";
}

// ---------------------------------------------------------------- Tab 4: CEC (remote test and bus status)

static void input_tab_remote(const input_state_t *in, int sel)
{
    (void)sel;
    CECRemote_HandleInput(in);
}

static const char *footer_tab_remote(void)
{
    if (CECRemote_IsTesting()) {
        return "Press buttons on the TV remote | (B) or hold the remote's BACK: End Test | (Y): Clear Log";
    }
    return "(A): Start Remote Test | (Y): Clear Event Log | Hold (BACK): Exit";
}

// ---------------------------------------------------------------- Tab 5: Status
//
// What is running (this app, the board's firmware, the board) and what it is doing, refreshed
// every second.

static void render_tab_status(void)
{
    int row = 5;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "SYSTEM:");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  App      : X-HD-Config v%s | built %s %s",
                      APP_VERSION, __DATE__, __TIME__);
    if (!g_state.xhd_present) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "  Firmware : no board answering at SMBus 0xD2");
        return;
    }
    if (!g_state.about_ok) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Firmware : v%s | build info not readable (READ_ABOUT)",
                          g_state.fw_ver_str);
        row++;
    } else {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Firmware : v%s | git %s | built %s",
                          g_state.fw_ver_str, g_state.about.git[0] ? g_state.about.git : "N/A",
                          g_state.about.built[0] ? g_state.about.built : "N/A");
        const uint8_t *u = g_state.about.mcu_uid;
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Board    : ADV7511 ID 0x%02X Rev 0x%02X | MCU UID %02X%02X%02X%02X-%02X%02X%02X%02X-%02X%02X%02X%02X",
                          g_state.about.chip_id, g_state.about.chip_rev,
                          u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11]);
    }

    row++;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "VIDEO (refreshed every second):");
    if (!g_state.vi_ok) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "  -- not read: the board did not answer READ_VIDEO_INFO --");
    } else if (!(g_state.vi.flags & XHD_VI_FLAG_APPLIED)) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Sending  : no mode applied");
    } else {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Sending  : %ux%u%c @ %.2f Hz | %s | VIC %u declared",
                          g_state.vi.width, g_state.vi.height,
                          (g_state.vi.flags & XHD_VI_FLAG_INTERLACED) ? 'i' : 'p',
                          (float)g_state.vi.refresh_mhz / 1000.0f,
                          XhdName_ColorSpace(g_state.vi.color_space), g_state.vi.wire_vic);
    }
    if (!g_state.tlm_ok) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "  -- telemetry not read: the board did not answer READ_TELEMETRY --");
        return;
    }
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Input    : %.3f MHz pixel clock (%s) | encoder %s (0x%02X)",
                      (float)g_state.tlm.pixel_clock_hz / 1000000.0f,
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_PLL_LOCKED) ? "PLL locked" : "PLL acquiring",
                      XhdName_Encoder(g_state.tlm.probed_encoder), g_state.tlm.probed_encoder);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Mode     : VIC %u from %s | the chip's detector sees VIC %u",
                      g_state.tlm.arb_target_vic, XhdName_ArbSource(g_state.tlm.arb_source),
                      g_state.tlm.detected_input_vic);

    row++;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "CONNECTION:");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Hot Plug : %-12s | Monitor Sense: %-8s | Video out: %-3s | Audio out: %s",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_HPD) ? "CONNECTED" : "DISCONNECTED",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_MONITOR_SENSE) ? "ACTIVE" : "INACTIVE",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_VIDEO_UNGATED) ? "ON" : "OFF",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_AUDIO_ON) ? "ON" : "OFF");

    row++;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "COUNTERS SINCE THE BOARD STARTED:");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Uptime %u s | Mode changes %u | PLL losses %u | TMDS gate cycles %u | Detector blind %u",
                      g_state.tlm.uptime_s, g_state.tlm.mode_change_count, g_state.tlm.pll_loss_count,
                      g_state.tlm.gate_cycle_count, g_state.tlm.blind_event_count);
}

static void enter_tab_status(void)
{
    read_video_info();
    g_state.tlm_ok = smbus_read_telemetry(&g_state.tlm);
}

// ---------------------------------------------------------------- Tab 6: EDID Inspector

static void input_tab_edid(const input_state_t *in, int sel)
{
    (void)sel;
    EDID_HandleInput(in);
}

static void enter_tab_edid(void)
{
    if (!EDID_HasData()) {
        EDID_RefreshFromBoard();
    }
}

// ---------------------------------------------------------------- Tab 7: EEPROM & Flash

enum { E_APPLY_EEPROM, E_SAVE, E_RELOAD, E_DEFAULTS, E_EXIT, E_ITEMS };

static void render_tab_eeprom_flash(void)
{
    const int sel = g_state.selected_item[TAB_EEPROM];
    int row = 5;

    // Motherboard EEPROM configuration (rows 5-8)
    const xbox_eeprom_info_t *eeprom = EEPROM_GetInfo();
    const edid_parsed_t *edid = EDID_GetParsed();
    char sinkAudio[48];
    // EDID_GetParsed() is NULL until an EDID has been read (no board, no TV, a failed read).
    if (!edid || !edid->valid) {
        snprintf(sinkAudio, sizeof(sinkAudio), "no EDID -- audio kept");
    } else if (!edid->is_hdmi || (!edid->basic_audio && edid->max_audio_channels == 0)) {
        snprintf(sinkAudio, sizeof(sinkAudio), "no audio -- audio kept");
    } else {
        snprintf(sinkAudio, sizeof(sinkAudio), "%uch%s%s",
                 edid->max_audio_channels ? edid->max_audio_channels : 2,
                 edid->dolby_digital ? " AC3" : "", edid->dts ? " DTS" : "");
    }
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "XBOX MOTHERBOARD EEPROM VIDEO & AUDIO:  (TV audio: %s)", sinkAudio);

    if (eeprom->pal_detected) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_BRIGHT,
                          "  Region: %s -> [SWITCH TO NTSC-M RECOMMENDED (Unlocks HD)]",
                          EEPROM_GetVideoStandardStr(eeprom->video_standard));
    } else {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Region: %s (HDTV Dashboard Modes Active)",
                          EEPROM_GetVideoStandardStr(eeprom->video_standard));
    }

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                      "  Video : Cur [480p:%s 720p:%s 1080i:%s 16:9:%s]  Target [480p:%s 720p:%s 1080i:%s 16:9:%s]",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                      "  Audio : Cur [%s AC3:%s DTS:%s]  Target [%s AC3:%s DTS:%s]",
                      EepromAudio_ModeName(eeprom->current_audio_flags),
                      (eeprom->current_audio_flags & EEPROM_AUDIO_ENABLE_AC3) ? "ON" : "OFF",
                      (eeprom->current_audio_flags & EEPROM_AUDIO_ENABLE_DTS) ? "ON" : "OFF",
                      EepromAudio_ModeName(eeprom->recommended_audio_flags),
                      (eeprom->recommended_audio_flags & EEPROM_AUDIO_ENABLE_AC3) ? "ON" : "OFF",
                      (eeprom->recommended_audio_flags & EEPROM_AUDIO_ENABLE_DTS) ? "ON" : "OFF");

    // Flash page 63 and system actions (rows 9-15, confirm prompt 16)
    Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "SAVED SETTINGS (BOARD FLASH) & ACTIONS:");

    Terminal_PrintItem(row++, sel == E_APPLY_EEPROM, "%s", eeprom->pal_detected
                       ? "Apply Recommended EEPROM (Switch PAL->NTSC-M & Enable HD)"
                       : "Apply Recommended EEPROM Settings (from EDID)");
    Terminal_PrintItem(row++, sel == E_SAVE, "Save Current Settings to the Board");
    Terminal_PrintItem(row++, sel == E_RELOAD, "Reload Saved Settings from the Board");
    Terminal_PrintItem(row++, sel == E_DEFAULTS, "Restore Factory Defaults & Save to Flash");
    Terminal_PrintItem(row++, sel == E_EXIT, "Exit to Dashboard (Reboot / Launch)");

    if (confirm_armed(&s_confirm, CONF_DEFAULTS)) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER,
                          "  -> Press (A) again to confirm Restore Factory Defaults, (B) to cancel");
    } else if (confirm_armed(&s_confirm, CONF_EEPROM)) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER,
                          "  -> Press (A) again to WRITE the console EEPROM, (B) to cancel");
    } else if (confirm_armed(&s_confirm, CONF_EEPROM_REBOOT)) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER,
                          "  -> EEPROM written. (A) reboots the console now so the dashboard and the board use it, (B) later");
    }
}

// ---- Leaving the app (dashboard or console reboot), with the unsaved-changes question

typedef enum { LEAVE_DASHBOARD, LEAVE_REBOOT } leave_t;
static leave_t s_pending_leave;

static void do_leave(leave_t how)
{
    if (how == LEAVE_REBOOT) {
        UI_RunCRTCollapse();
        FwUpdate_RebootConsole();   // doesn't return
    }
    UI_RunCRTCollapseAndExit();     // doesn't return
}

enum { LEAVE_SAVE, LEAVE_DISCARD, LEAVE_STAY, LEAVE_ITEMS };

// With unsaved changes on a board that can save them, ask first; otherwise leave now.
static void request_leave(leave_t how)
{
    if (!s_unsaved || !g_state.xhd_present) {
        do_leave(how);
        return;
    }
    s_pending_leave = how;
    static const char *items[LEAVE_ITEMS];
    items[LEAVE_SAVE]    = how == LEAVE_REBOOT ? "Save to the board, then reboot" : "Save to the board, then exit";
    items[LEAVE_DISCARD] = how == LEAVE_REBOOT ? "Reboot without saving (changes are lost)"
                                               : "Exit without saving (try them; a reboot undoes them)";
    items[LEAVE_STAY]    = "Stay here";
    Picklist_Open(PICK_ID_LEAVE, "UNSAVED CHANGES:", items, LEAVE_ITEMS, -1, 9, 300);
}

void App_RequestReboot(void)
{
    request_leave(LEAVE_REBOOT);
}

// ---- Tab 7 actions

static bool save_to_board(void)
{
    if (smbus_trigger_action(XHD_ACTION_SAVE_FLASH)) {
        s_unsaved = false;
        set_alert("SUCCESS: Settings & calibration saved to the board");
        return true;
    }
    set_alert("ERROR: The board did not save the settings (no SMBus answer)");
    return false;
}

static void handle_action_save_flash(void)
{
    save_to_board();
}

static void handle_action_apply_eeprom(void)
{
    if (confirm_armed(&s_confirm, CONF_EEPROM_REBOOT)) {
        confirm_clear(&s_confirm);
        request_leave(LEAVE_REBOOT);
        return;
    }
    if (!confirm_armed(&s_confirm, CONF_EEPROM)) {
        confirm_arm(&s_confirm, CONF_EEPROM);
        set_status("Press (A) again to CONFIRM: write recommended settings to the console EEPROM");
        return;
    }
    confirm_clear(&s_confirm);
    const bool ok = EEPROM_ApplyRecommended(EDID_GetParsed());
    const xbox_eeprom_info_t *info = EEPROM_GetInfo();
    set_alert(info->status_str[0] ? info->status_str
                                  : (ok ? "SUCCESS: EEPROM updated" : "ERROR: Failed to update Motherboard EEPROM"));
    if (ok) {
        // The dashboard reads the EEPROM at boot, and the X-HD captures it as the console
        // starts: neither sees the change until a power cycle.
        confirm_arm(&s_confirm, CONF_EEPROM_REBOOT);
    }
}

// After a reload or a restore: success only if the board's settings could be read back.
static void after_refresh(const char *success)
{
    if (g_state.cfg_ok && g_state.vi_ok && g_state.tlm_ok && g_state.about_ok) {
        set_alert(success);
    }   // else refresh_all_from_board() has already said what couldn't be read
}

static void handle_action_reload_flash(void)
{
    if (smbus_trigger_action(XHD_ACTION_RELOAD_FLASH)) {
        refresh_all_from_board();
        after_refresh("SUCCESS: Saved settings reloaded from the board");
    } else {
        set_alert("ERROR: The board did not reload the saved settings");
    }
}

static void handle_action_restore_defaults(void)
{
    if (!confirm_armed(&s_confirm, CONF_DEFAULTS)) {
        confirm_arm(&s_confirm, CONF_DEFAULTS);
        set_status("Press (A) again to CONFIRM: Restore factory defaults to Flash");
        return;
    }
    confirm_clear(&s_confirm);
    if (smbus_trigger_action(XHD_ACTION_RESTORE_DEFAULTS)) {
        refresh_all_from_board();
        after_refresh("SUCCESS: Restored factory defaults & saved to Flash!");
    } else {
        set_alert("ERROR: Restore factory defaults failed");
    }
}

static void input_tab_eeprom_flash(const input_state_t *in, int sel)
{
    if (in->a) {
        switch (sel) {
        case E_APPLY_EEPROM: handle_action_apply_eeprom(); break;
        case E_SAVE:         handle_action_save_flash(); break;
        case E_RELOAD:       handle_action_reload_flash(); break;
        case E_DEFAULTS:     handle_action_restore_defaults(); break;
        case E_EXIT:         request_leave(LEAVE_DASHBOARD); break;
        }
    }
    if ((in->b || in->back) && confirm_any(&s_confirm)) {
        cancel_confirms();
        set_status("Action cancelled");
    }
}

static void enter_tab_eeprom_flash(void)
{
    EEPROM_Refresh(EDID_GetParsed());
}

// ---------------------------------------------------------------- Tab 9: Help

static void input_tab_help(const input_state_t *in, int sel)
{
    (void)sel;
    About_HandleInput(in);
}

// ---------------------------------------------------------------- Tab 8: Firmware Update

static void input_tab_firmware(const input_state_t *in, int sel)
{
    (void)sel;
    Firmware_HandleInput(in);
}

// ----------------------------------------------------------------------------
// The tab table
// ----------------------------------------------------------------------------

// What (X) and (Y) do on a tab that doesn't use them itself: save to and reload from the board.
enum { KEY_SAVE = 1, KEY_RELOAD = 2 };

typedef struct {
    const char *strip_name;     // its name in the tab strip, shown while it's the active one
    int  items;                 // Up/Down cursor items; 0 when the tab moves its own cursor
    unsigned keys;              // KEY_SAVE / KEY_RELOAD on (X) / (Y)
    bool poll;                  // refreshed every second while shown (Tabs 3 and 5)
    void (*render)(void);
    void (*input)(const input_state_t *in, int sel);
    void (*enter)(void);        // on switching to the tab (NULL: nothing)
    void (*leave)(void);        // on switching away (NULL: nothing)
    const char *(*footer)(void);  // NULL: `footer_text`
    const char *footer_text;
} tab_t;

static const tab_t kTabs[NUM_TABS] = {
    [TAB_VIDEO] = {
        "SETTINGS", SET_MAIN_COUNT, KEY_SAVE | KEY_RELOAD, false,
        render_tab_video, input_tab_video, NULL, leave_tab_video,
        footer_tab_video, NULL },
    [TAB_POSITION] = {
        "ALIGNMENT", POS_ITEMS, KEY_SAVE, false,
        render_tab_position, input_tab_position, NULL, NULL,
        footer_tab_position, NULL },
    [TAB_PICTURE] = {
        "PICTURE", P_ITEMS, KEY_SAVE | KEY_RELOAD, true,
        render_tab_picture, input_tab_picture, enter_tab_picture, leave_tab_picture,
        footer_tab_picture, NULL },
    [TAB_CEC] = {
        "CEC", 0, 0, false,
        CECRemote_Render, input_tab_remote, NULL, NULL,
        footer_tab_remote, NULL },
    [TAB_STATUS] = {
        "STATUS", 0, 0, true,
        render_tab_status, NULL, enter_tab_status, NULL,
        NULL, "Refreshed every second | Hold (BACK): Exit" },
    [TAB_EDID] = {
        "EDID", 0, 0, false,
        EDID_Render, input_tab_edid, enter_tab_edid, NULL,
        NULL, "(X): Toggle Hex Dump | (A)/TRIGGERS: Switch Block | (Y): Refresh Sink EDID" },
    [TAB_EEPROM] = {
        "EEPROM", E_ITEMS, KEY_SAVE | KEY_RELOAD, false,
        render_tab_eeprom_flash, input_tab_eeprom_flash, enter_tab_eeprom_flash, NULL,
        NULL, "(A): Run Selected | (B): Cancel | (X): Save Flash | (Y): Reload | Hold (BACK): Exit" },
    [TAB_FIRMWARE] = {
        "FIRMWARE", 0, 0, false,
        Firmware_Render, input_tab_firmware, Firmware_OnEnter, NULL,
        Firmware_Footer, NULL },
    [TAB_HELP] = {
        "HELP", 0, 0, false,
        About_Render, input_tab_help, NULL, NULL,
        NULL, "(A): Open/Close | (B): Close | (X): All Sections | TRIGGERS: Scroll" },
};

static void switch_tab(tab_id_t to)
{
    const tab_id_t from = g_state.active_tab;
    if (to == from) return;
    cancel_confirms();
    if (kTabs[from].leave) kTabs[from].leave();
    const bool from_mode = from == TAB_POSITION || from == TAB_PICTURE;
    const bool to_mode = to == TAB_POSITION || to == TAB_PICTURE;
    if (from_mode && !to_mode) restore_launch_mode();
    g_state.active_tab = to;
    if (kTabs[to].enter) kTabs[to].enter();
}

// ----------------------------------------------------------------------------
// Pick lists (A on a setting with more than two values)
// ----------------------------------------------------------------------------

static void apply_pick(int id, int chosen)
{
    switch (id) {
    case PICK_ID_COLOR_SPACE:
        set_color_space((uint8_t)chosen);
        break;
    case PICK_ID_ASPECT:
        if (setting_written(smbus_set_aspect_ratio((uint8_t)chosen), "Aspect Ratio changed (Applies to SD modes)")) {
            g_state.aspect_ratio = (uint8_t)chosen;
        }
        break;
    case PICK_ID_SCAN:
        if (setting_written(smbus_set_scan_mode((uint8_t)chosen), "Scan Mode changed (Live reapply)")) {
            g_state.scan_mode = (uint8_t)chosen;
        }
        break;
    case PICK_ID_CLOCK_PHASE:
        if (setting_written(smbus_set_clock_phase((uint8_t)chosen), "Clock Phase trim adjusted")) {
            g_state.clock_phase = (uint8_t)chosen;
        }
        break;
    case PICK_ID_CEC_STANDBY:
        if (setting_written(smbus_set_cec_standby((uint8_t)chosen), "TV at Power-Off behaviour changed")) {
            g_state.cec_standby_mode = (uint8_t)chosen;
        }
        break;
    case PICK_ID_VIDEO_MODE:
        switch_video_mode(chosen);
        break;
    case PICK_ID_SPD:
        if (chosen == s_spdEditItem) {
            OSK_Open(g_state.spd_name);
        } else if (chosen == s_spdCustomItem) {
            if (setting_written(smbus_write_spd(s_customSpd), "SPD TV Label restored to custom entry")) {
                memcpy(g_state.spd_name, s_customSpd, sizeof(g_state.spd_name));
            }
        } else if (chosen >= 0 && chosen < SPD_PRESET_COUNT) {
            char preset[25];
            SpdPreset_Name(chosen, preset);
            if (setting_written(smbus_write_spd(preset), "SPD TV Input Name Preset committed")) {
                memcpy(g_state.spd_name, preset, sizeof(g_state.spd_name));
            }
        }
        break;
    case PICK_ID_PIC_PROFILE:
        pic_pick_profile(chosen);
        break;
    case PICK_ID_FW_IMAGE:
        Firmware_PickImage(chosen);
        break;
    case PICK_ID_LEAVE:
        if (chosen == LEAVE_DISCARD || (chosen == LEAVE_SAVE && save_to_board())) {
            do_leave(s_pending_leave);
        }   // LEAVE_STAY, or the save failed: its error is on the status line
        break;
    default:
        break;
    }
}

// ----------------------------------------------------------------------------
// Input Processing
// ----------------------------------------------------------------------------
//
// Everywhere: Up/Down pick an item, (A) changes or runs it (a list for settings with more than
// two values), (B) cancels, White/Black switch tabs (the TV remote's Left/Right too), (X) saves
// and (Y) reloads on the settings tabs, hold (BACK) exits. Left/Right only act where the D-pad
// is the content: moving the picture, a picture control, the full-screen pattern, the keyboard.

static void process_input(const input_state_t *in)
{
    // A firmware job owns the screen and the bus until it ends.
    if (Firmware_IsBusy()) {
        return;
    }
    // After an update only the reboot is left: no other tab, no other action.
    if (Firmware_RebootPending()) {
        g_state.active_tab = TAB_FIRMWARE;
        Firmware_HandleInput(in);
        return;
    }
    if (Picklist_IsActive()) {
        int chosen = 0;
        if (Picklist_HandleInput(in, &chosen) == PICK_CHOSEN) {
            apply_pick(Picklist_Id(), chosen);
        }
        return;
    }
    if (s_pic_full || (g_state.active_tab == TAB_PICTURE && s_pic_adjust)) {
        if (s_pic_full) input_picture_full(in); else input_picture_adjust(in);
        if (in->x) {
            handle_action_save_flash();
        }
        return;
    }
    if (g_state.active_tab == TAB_POSITION && s_pos_adjust) {
        input_position_adjust(in);
        if (in->x) {
            handle_action_save_flash();
        }
        return;
    }

    // Tab switching via White / Black buttons
    if (in->white || in->black) {
        switch_tab((tab_id_t)((g_state.active_tab + (in->black ? 1 : NUM_TABS - 1)) % NUM_TABS));
        return;
    }

    const tab_t *tab = &kTabs[g_state.active_tab];
    if (in->x && (tab->keys & KEY_SAVE)) {
        handle_action_save_flash();
        return;
    }
    if (in->y && (tab->keys & KEY_RELOAD)) {
        handle_action_reload_flash();
        return;
    }

    // Tab 3's white balance page and Tab 1's Advanced page move their own cursors.
    if (g_state.active_tab == TAB_PICTURE && s_pic_wb) {
        input_picture_wb(in);
        return;
    }
    if (g_state.active_tab == TAB_VIDEO && s_set_adv) {
        input_settings_adv(in);
        return;
    }

    int *sel = &g_state.selected_item[g_state.active_tab];
    if (in->up && *sel > 0) {
        (*sel)--;
        cancel_confirms();
    }
    if (in->down && *sel < tab->items - 1) {
        (*sel)++;
        cancel_confirms();
    }
    if (tab->input) {
        tab->input(in, *sel);
    }
}

// The footer's key row for the current screen.
static const char *footer_keys(void)
{
    if (Picklist_IsActive()) {
        return "D-PAD: Choose | (A): Select | (B): Cancel";
    }
    const tab_t *tab = &kTabs[g_state.active_tab];
    return tab->footer ? tab->footer() : tab->footer_text;
}

// ----------------------------------------------------------------------------
// Header and footer
// ----------------------------------------------------------------------------

static void draw_header(void)
{
    static const char *names[NUM_TABS];
    for (int i = 0; i < NUM_TABS; i++) {
        names[i] = kTabs[i].strip_name;
    }
    char board[48];
    DWORD color = COLOR_PHOSPHOR_AMBER;
    switch (g_state.board.kind) {
    case FW_BOARD_XHD_APP:
        snprintf(board, sizeof(board), "FW: v%s", g_state.fw_ver_str);
        color = COLOR_PHOSPHOR_NORMAL;
        break;
    case FW_BOARD_XHD_BOOT: snprintf(board, sizeof(board), "BOARD IN UPDATE MODE"); break;
    case FW_BOARD_HDPLUS:   snprintf(board, sizeof(board), "HD+ (STOCK FIRMWARE)"); break;
    default:                snprintf(board, sizeof(board), "NO BOARD"); break;
    }
    const terminal_header_t h = {
        names, NUM_TABS, (int)g_state.active_tab, board, color,
        s_unsaved && g_state.xhd_present,
    };
    Terminal_DrawHeader(&h);
}

// The message line is empty with nothing to report: the header shows the board and whether
// settings are unsaved.
static void draw_footer(void)
{
    Terminal_DrawFooter(footer_keys(), g_state.status_msg, s_status_alert);
}

// ----------------------------------------------------------------------------
// Main Application Entry Point
// ----------------------------------------------------------------------------

int main(void)
{
    memset(&g_state, 0, sizeof(app_state_t));
    g_state.active_tab = TAB_VIDEO;

    // Before anything opens a file: Media, the firmware images and backups are on D:.
    AppDir_MountD();

    About_Init();
    CECRemote_Init();
    EEPROM_Init();

    // Initialize Xbox controller and Direct3D terminal
    input_init();

    // Open in the best mode the dashboard settings allow that the console accepts.
    VideoMode_Init();
    s_launch_mode = -1;
    for (int i = 0; i < VideoMode_GetCount() && s_launch_mode < 0; i++) {
        const video_mode_info_t *m = VideoMode_Get(i);
        if (Terminal_Init(m->width, m->height, m->d3d_flags, m->refresh_rate)) {
            s_launch_mode = i;
        }
    }
    if (s_launch_mode < 0) {
        // No mode the console accepts: nothing can be drawn, so go back to the dashboard
        // rather than sit on a black screen.
        XLaunchNewImage(NULL, NULL);
        for (;;) Sleep(1000);
    }
    VideoMode_SetCurrentIndex(s_launch_mode);

    // Connect to X-HD over SMBus and read initial state
    refresh_all_from_board();

    // No X-HD application answering (an HD+, an interrupted update, or nothing): start on
    // the firmware tab.
    Firmware_Init();
    if (Firmware_WantsFocus()) {
        g_state.active_tab = TAB_FIRMWARE;
        Firmware_OnEnter();
    }

    // Initial frame render (fill both back and front buffers)
    s_dirty_frames = 2;

    // Reactive Render-on-Demand Loop
    for (;;) {
        DWORD now = GetTickCount();

        // 1. Poll Xbox Controller Input
        input_state_t in;
        input_poll(&in);
        // TV remote (CEC relay): its keys count as controller taps everywhere but the remote
        // tester tab. Not while a firmware job owns the bus or the reboot lock is on.
        const bool fw_locked = Firmware_IsBusy() || Firmware_RebootPending();
        if (CECRemote_Update(g_state.active_tab == TAB_CEC, !fw_locked)) {
            s_dirty_frames = 2;
        }
        // The remote's Left/Right switch tabs, except where the D-pad is the content.
        const bool dpad_screen = OSK_IsActive() || s_pic_full || Picklist_IsActive() ||
                                 (g_state.active_tab == TAB_POSITION && s_pos_adjust) ||
                                 (g_state.active_tab == TAB_PICTURE && s_pic_adjust);
        CECRemote_ApplyNav(&in, !dpad_screen);
        if (fw_locked) {
            in.exit_requested = false;   // never leave mid-flash, or before the reboot after one
            in.back_hold_ms = 0;
        }
        if (in.exit_requested && !Picklist_IsActive()) {
            request_leave(LEAVE_DASHBOARD);   // returns only to ask about unsaved changes
            in.exit_requested = false;
            s_dirty_frames = 2;
        }
        // Past a tap's length, say what holding BACK does. Cleared on release before the
        // tap action runs, so a tap's own status message isn't lost.
        static bool s_exit_hint = false;
        if (in.back_hold_ms >= 300 && !s_exit_hint) {
            s_exit_hint = true;
            set_status("Keep holding (BACK) to exit to the dashboard...");
        } else if (in.back_hold_ms == 0 && s_exit_hint) {
            s_exit_hint = false;
            set_status("");
        }
        if (input_has_action(&in)) {
            s_dirty_frames = 2;
            // An alert stays until the next press; this one may set a new alert of its own.
            if (s_status_alert) {
                set_status("");
            }
        }

        // 2. Process Input (the unsaved-changes question can open over the keyboard)
        if (OSK_IsActive() && !Picklist_IsActive()) {
            bool committed = false;
            char newSpd[25];
            if (OSK_HandleInput(&in, &committed, newSpd)) {
                s_dirty_frames = 2;
                if (committed) {
                    memcpy(s_customSpd, newSpd, 25);
                    s_hasCustomSpd = true;
                    if (setting_written(smbus_write_spd(newSpd), "SPD TV Label applied to the board -- (X) saves it")) {
                        memcpy(g_state.spd_name, newSpd, 25);
                    }
                }
            }
        } else {
            process_input(&in);
        }

        // 3. Firmware jobs: progress, and a board refresh once one has changed the firmware.
        bool board_changed = false;
        if (Firmware_Update(&board_changed)) {
            s_dirty_frames = 2;
        }
        if (board_changed) {
            s_just_flashed = true;
            refresh_all_from_board();
            s_just_flashed = false;
        }

        // 4. Poll the live tabs at 1 Hz: Tab 3's raster block, Tab 5's raster block and telemetry. With no board
        // answering, look for one every 2 s instead (not on Tab 8, which has its own rescan,
        // and not while a firmware job owns the bus).
        if (!g_state.xhd_present) {
            static DWORD s_last_detect;
            if (g_state.active_tab != TAB_FIRMWARE && !fw_locked && now - s_last_detect >= 2000) {
                s_last_detect = now;
                refresh_all_from_board();
                if (g_state.xhd_present) {
                    set_status("Board found");
                }
                s_dirty_frames = 2;
            }
        } else if (kTabs[g_state.active_tab].poll && now - g_state.last_poll_tick >= 1000) {
            g_state.last_poll_tick = now;
            kTabs[g_state.active_tab].enter();
            s_dirty_frames = 2;
        }

        // 5. Retro Phosphor Cursor Blink Phase Flip (~2 Hz: 450ms toggle)
        if (!s_pic_full) {
            static bool s_last_blink_phase = false;
            bool cur_blink_phase = ((now / 450) % 2 == 0);
            if (cur_blink_phase != s_last_blink_phase) {
                s_last_blink_phase = cur_blink_phase;
                s_dirty_frames = 2;
            }
        }

        // 6. Status Message Timeout (alerts have none), and two-press confirmations lapsing
        if (g_state.status_msg[0] != '\0' && s_status_expire_tick != 0 && (LONG)(now - s_status_expire_tick) >= 0) {
            set_status("");
        }
        const int lapsed = s_confirm.id;
        if (confirm_expired(&s_confirm)) {
            set_status(lapsed == CONF_EEPROM_REBOOT
                       ? "Reboot the console later: the EEPROM change takes effect at the next boot"
                       : "Confirmation timed out: nothing was done");
        }

        // 7. Render on Demand
        if (s_dirty_frames > 0) {
            Terminal_BeginFrame();

            if (OSK_IsActive()) {
                OSK_Render();
            } else if (s_pic_full) {
                render_picture_full();
            } else {
                draw_header();
                kTabs[g_state.active_tab].render();
                draw_footer();
            }
            Picklist_Render();

            Terminal_EndFrame();
            s_dirty_frames--;
        } else {
            // Idle: Yield CPU to maintain ~100 Hz input polling responsiveness without spinning
            Sleep(10);
        }
    }

    return 0;
}
