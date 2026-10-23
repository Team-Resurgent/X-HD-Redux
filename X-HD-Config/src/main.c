#include "xbox_base.h"
#include "defines.h"
#include <stdio.h>
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
#include "ui_black_level.h"

#define NUM_TABS 8

static const char *kTabNames[NUM_TABS] = {
    "< 1. VIDEO SETTINGS >",
    "< 2. PICTURE ALIGNMENT >",
    "< 3. CEC & SPD CONFIG >",
    "< 4. CEC REMOTE TESTER >",
    "< 5. LIVE TELEMETRY >",
    "< 6. EDID INSPECTOR >",
    "< 7. FLASH & HARDWARE >",
    "< 8. ABOUT >"
};

// Application State
typedef struct {
    int active_tab;
    int selected_item[NUM_TABS];
    char status_msg[128];
    int status_timer;

    // Board detection
    bool xhd_present;
    uint8_t fw_mode;
    uint8_t fw_ver[4];
    char fw_ver_str[32];

    // Cached live structures
    XHD_VIDEO_INFO vi;
    XHD_TELEMETRY  tlm;
    XHD_ABOUT      about;
    xhd_user_config cfg;
    char           spd_name[25];

    // Local live settings
    uint8_t color_space;
    uint8_t aspect_ratio;
    uint8_t scan_mode;
    uint8_t wire_vic_mode;
    uint8_t clock_phase;
    int8_t  pos_h;
    int8_t  pos_v;
    bool    cec_clock;
    bool    cec_one_touch;
    bool    cec_standby;
    bool    audio_mute;
    bool    smbus_freeze;

    // Telemetry polling timers
    DWORD   last_poll_tick;
    DWORD   activity_frame;
} app_state_t;

static app_state_t g_state;

static const char *kSpdPresets[] = {
    "MSFT    Xbox            ",
    "MSFT    Microsoft Xbox  ",
    "MSFT    X-HD            ",
    "MSFT    Retro Consoles  "
};
#define NUM_SPD_PRESETS 4

static char s_customSpd[25] = "";
static bool s_hasCustomSpd = false;
static int  s_dirty_frames = 2;
static DWORD s_status_expire_tick = 0;

static void set_status(const char *msg)
{
    if (!msg || msg[0] == '\0') {
        g_state.status_msg[0] = '\0';
        s_status_expire_tick = 0;
        s_dirty_frames = 2;
        return;
    }
    strncpy(g_state.status_msg, msg, sizeof(g_state.status_msg) - 1);
    g_state.status_msg[sizeof(g_state.status_msg) - 1] = '\0';
    s_status_expire_tick = GetTickCount() + 3000;
    s_dirty_frames = 2;
}

// set_status() for a setting write: its message, or an error if the board didn't take it.
static void set_result(bool ok, const char *msg)
{
    set_status(ok ? msg : "ERROR: SMBus write failed (X-HD not responding)");
}

static void refresh_all_from_board(void)
{
    g_state.xhd_present = smbus_detect_board(&g_state.fw_mode, g_state.fw_ver);
    if (!g_state.xhd_present) {
        set_status("X-HD board not responding at SMBus 0xD2!");
        snprintf(g_state.fw_ver_str, sizeof(g_state.fw_ver_str), "N/A");
        return;
    }

    smbus_read_video_info(&g_state.vi);
    smbus_read_telemetry(&g_state.tlm);
    smbus_read_about(&g_state.about);
    smbus_read_spd(g_state.spd_name);
    if (g_state.spd_name[0] != '\0') {
        bool matchesPreset = false;
        for (int i = 0; i < NUM_SPD_PRESETS; i++) {
            if (strncmp(g_state.spd_name, kSpdPresets[i], 24) == 0) {
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

    snprintf(g_state.fw_ver_str, sizeof(g_state.fw_ver_str), "%u.%u.%u",
             g_state.fw_ver[0], g_state.fw_ver[1], g_state.fw_ver[2]);

    if (smbus_read_user_config(&g_state.cfg)) {
        g_state.color_space   = g_state.cfg.color_space;
        g_state.aspect_ratio  = g_state.cfg.aspect_ratio;
        g_state.scan_mode     = g_state.cfg.scan_mode;
        g_state.wire_vic_mode = g_state.cfg.wire_vic_mode;
        g_state.cec_clock     = (g_state.cfg.cec_clock_off == 0);
        g_state.cec_one_touch = (g_state.cfg.cec_one_touch_off == 0);
        g_state.cec_standby   = (g_state.cfg.cec_standby_off == 0);
        g_state.smbus_freeze  = (g_state.cfg.smbus_freeze == 1);

        uint8_t enc = g_state.cfg.active_encoder;
        if (enc < 3) {
            g_state.clock_phase = g_state.cfg.enc_cal[enc].clock_phase;
        }
    }

    g_state.pos_h = g_state.vi.pos_h;
    g_state.pos_v = g_state.vi.pos_v;

    uint8_t flags = 0;
    if (smbus_read_live_flags(&flags)) {
        g_state.audio_mute   = (flags & XHD_LIVE_FLAG_AUDIO_MUTE_HELD) != 0;
        g_state.smbus_freeze = (flags & XHD_LIVE_FLAG_FREEZE_ON) != 0;
    }

    set_status("Loaded configuration & telemetry from X-HD");
}

static const char *color_space_str(uint8_t cs)
{
    switch (cs) {
        case XHD_COLOR_SPACE_AUTO:              return "Auto (EDID / Sink Match)";
        case XHD_COLOR_SPACE_RGB_FULL:          return "RGB Full (0-255)";
        case XHD_COLOR_SPACE_RGB_LIMITED:       return "RGB Limited (16-235)";
        case XHD_COLOR_SPACE_YCBCR_PASSTHROUGH: return "YCbCr 4:4:4 Passthrough";
        case XHD_COLOR_SPACE_RGB_PASSTHROUGH:   return "RGB Direct Passthrough";
        case XHD_COLOR_SPACE_AUTO_SD_FULL:      return "Auto (SD Forced RGB Full)";
        default: return "Unknown";
    }
}

static const char *aspect_ratio_str(uint8_t ar)
{
    switch (ar) {
        case XHD_ASPECT_AUTO: return "Auto (Console / EEPROM Follow)";
        case XHD_ASPECT_16_9: return "16:9 Widescreen";
        case XHD_ASPECT_4_3:  return "4:3 Standard";
        default: return "Unknown";
    }
}

static const char *scan_mode_str(uint8_t sm)
{
    switch (sm) {
        case XHD_SCAN_AUTO: return "Auto (EDID IT Content Game Mode)";
        case XHD_SCAN_TV:   return "TV (Overscan Mode)";
        case XHD_SCAN_PC:   return "PC (Underscan 1:1 Pixel Mapping)";
        default: return "Unknown";
    }
}

static const char *encoder_name(uint8_t enc)
{
    switch (enc) {
        case 0x8A: return "Conexant CX25871";
        case 0xD4: return "Focus FS454";
        case 0xE0: return "Microsoft Xcalibur";
        default:   return "Unknown / Probing";
    }
}

static const char *arb_source_str(uint8_t src)
{
    switch (src) {
        case 0: return "Hold Previous";
        case 1: return "BIOS Mode Packet";
        case 2: return "BIOS Pending";
        case 3: return "Path A (Hardware Geometry)";
        case 4: return "ADV7511 Line Detector";
        case 5: return "Width Debounce Correction";
        case 6: return "Blind Fallback Arbiter";
        default: return "Unknown";
    }
}

// ----------------------------------------------------------------------------
// Render Views (720p Overlap-Free Layout)
// ----------------------------------------------------------------------------

static void render_tab_video(void)
{
    int sel = g_state.selected_item[0];
    int row = 6;

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "VIDEO & ENCODER REAL-TIME SETTINGS:");

    // Item 0: Color Space
    Terminal_PrintRow(row++, (sel == 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Colour Space   : %s", (sel == 0) ? ">" : " ", color_space_str(g_state.color_space));

    // Item 1: Aspect Ratio
    Terminal_PrintRow(row++, (sel == 1) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Aspect Ratio   : %s", (sel == 1) ? ">" : " ", aspect_ratio_str(g_state.aspect_ratio));

    // Item 2: Scan Mode
    Terminal_PrintRow(row++, (sel == 2) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Scan Mode      : %s", (sel == 2) ? ">" : " ", scan_mode_str(g_state.scan_mode));

    // Item 3: Wire VIC Mode
    Terminal_PrintRow(row++, (sel == 3) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Wire-VIC Mode  : %s", (sel == 3) ? ">" : " ",
                      g_state.wire_vic_mode ? "1 (Chip-Auto Detection)" : "0 (Honest CEA Standard)");

    // Item 4: Clock Phase Trim
    Terminal_PrintRow(row++, (sel == 4) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Clock Phase    : %d (0.4ns trim step, raw reg 0xBA)", (sel == 4) ? ">" : " ", g_state.clock_phase);

    // Item 5: Audio Mute
    Terminal_PrintRow(row++, (sel == 5) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Audio Output   : %s", (sel == 5) ? ">" : " ",
                      g_state.audio_mute ? "[MUTED]" : "[ACTIVE]");

    // Item 6: Freeze Mode
    Terminal_PrintRow(row++, (sel == 6) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s Standalone Frz : %s", (sel == 6) ? ">" : " ",
                      g_state.smbus_freeze ? "[ENABLED - Standalone Mode]" : "[DISABLED - Normal BIOS Track]");

    if (Terminal_GetHeight() > 480) {
        row++;
        Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "CURRENT ACTIVE RASTER STATUS (From ADV7511 & Line Detector):");

        if (!(g_state.vi.flags & XHD_VI_FLAG_APPLIED)) {
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Active Resolution : No mode applied");
        } else {
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Active Resolution : %ux%u%c @ %.2f Hz",
                              g_state.vi.width, g_state.vi.height,
                              (g_state.vi.flags & XHD_VI_FLAG_INTERLACED) ? 'i' : 'p',
                              (float)g_state.vi.refresh_mhz / 1000.0f);
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  On-Wire ColorSpace: %s", color_space_str(g_state.vi.color_space));
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Declared Wire VIC : VIC %u (CEA Standard)", g_state.vi.wire_vic);
        }
    }
}

static void render_tab_position(void)
{
    int curModeIdx = VideoMode_GetCurrentIndex();
    const video_mode_info_t *curMode = VideoMode_Get(curModeIdx);

    Terminal_PrintRow(5, COLOR_PHOSPHOR_DIM, "PICTURE POSITION & CENTERING CALIBRATION:");

    // Offsets are stored per timing profile; the board picks the profile from the console
    // mode it sees, reported here as the mode index (0xFF = standalone, no console packet).
    if (Terminal_GetWidth() <= 640) {
        Terminal_PrintRow(6, COLOR_PHOSPHOR_BRIGHT,
                          "Mode: %s | Idx 0x%02X | H %+d, V %+d",
                          curMode ? curMode->name : "Def",
                          g_state.vi.console_mode_index,
                          g_state.pos_h, g_state.pos_v);
    } else {
        Terminal_PrintRow(6, COLOR_PHOSPHOR_BRIGHT,
                          "  Mode: %s | Console Mode Index: 0x%02X | Offset: H %+d px, V %+d lines",
                          curMode ? curMode->name : "Default",
                          g_state.vi.console_mode_index,
                          g_state.pos_h, g_state.pos_v);
    }

    // Hardware-aligned reticle centered at physical screen center
    Terminal_DrawAlignmentReticle(g_state.pos_h, g_state.pos_v);
}

static void render_tab_cec_spd(void)
{
    int sel = g_state.selected_item[2];
    int row = 6;

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "CEC HDMI CONSUMER ELECTRONICS & SPD INFOFRAME:");

    // Item 0: CEC Clock
    Terminal_PrintRow(row++, (sel == 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s CEC Clock 12MHz Out : %s", (sel == 0) ? ">" : " ",
                      g_state.cec_clock ? "[ENABLED - Driven on PA8 / Pin 29]" : "[DISABLED]");

    // Item 1: One Touch Play
    Terminal_PrintRow(row++, (sel == 1) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s CEC One Touch Play  : %s", (sel == 1) ? ">" : " ",
                      g_state.cec_one_touch ? "[ENABLED - Auto-wake & switch TV input]" : "[DISABLED]");

    // Item 2: CEC Standby
    Terminal_PrintRow(row++, (sel == 2) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s CEC Shutdown Standby: %s", (sel == 2) ? ">" : " ",
                      g_state.cec_standby ? "[ENABLED - TV Standby on Console Power-Off]" : "[DISABLED]");

    // Item 3: SPD Name Preset / OSK Editor
    Terminal_PrintRow(row++, (sel == 3) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s HDMI SPD TV Label   : \"%s\"   [(A) Edit in OSK | (L/R) Preset]",
                      (sel == 3) ? ">" : " ", g_state.spd_name);

    row++;
    Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "CEC BUS CONTROLLER STATUS (Opcode 18):");

    static uint8_t s_cached_cec_st = 0;
    static DWORD s_last_cec_poll = 0;
    DWORD now_cec = GetTickCount();
    if (now_cec - s_last_cec_poll >= 150) {
        s_last_cec_poll = now_cec;
        smbus_read_cec_status(&s_cached_cec_st);
    }
    uint8_t cec_st = s_cached_cec_st;

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Transmitter State  : %s",
                      (cec_st & XHD_CEC_STATUS_TX_BUSY) ? "TX BUSY (Transmitting)" : "IDLE");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Last Frame Acked   : %s",
                      (cec_st & XHD_CEC_STATUS_LAST_TX_ACKED) ? "YES (Sink Acknowledged)" : "NO / Pending");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Arbitration Status : %s",
                      (cec_st & XHD_CEC_STATUS_TX_LOST_ARB) ? "LOST (Collision detected)" : "OK");
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Receiver Frame Q   : %s",
                      (cec_st & XHD_CEC_STATUS_RX_PENDING) ? "PENDING FRAME IN BUFFER" : "Empty");
}

static void render_tab_telemetry(void)
{
    int row = 6;

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "LIVE HARDWARE TELEMETRY & DIAGNOSTICS STREAM:");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Measured Pixel Clock : %.3f MHz (%s)",
                      (float)g_state.tlm.pixel_clock_hz / 1000000.0f,
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_PLL_LOCKED) ? "PLL LOCKED" : "ACQUIRING...");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Target Arbiter VIC   : VIC %u (Decision Source: %s)",
                      g_state.tlm.arb_target_vic, arb_source_str(g_state.tlm.arb_source));

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Line Detector Input  : VIC %u (0x3E hardware detect)",
                      g_state.tlm.detected_input_vic);

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Probed Video Encoder : %s (0x%02X)",
                      encoder_name(g_state.tlm.probed_encoder), g_state.tlm.probed_encoder);

    row++;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "HARDWARE STATUS & INTEGRITY FLAGS:");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Hot Plug Detect: %-14s | Monitor Sense: %s",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_HPD) ? "CONNECTED" : "DISCONNECTED",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_MONITOR_SENSE) ? "ACTIVE" : "INACTIVE");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Video Ungated  : %-14s | HDMI Audio On: %s",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_VIDEO_UNGATED) ? "YES" : "NO",
                      (g_state.tlm.hw_flags & XHD_TLM_FLAG_AUDIO_ON) ? "YES" : "NO");

    row++;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "RUNNING COUNTERS & LIFETIME METRICS:");

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  Board Uptime   : %-10u s | Applied Mode Changes: %u",
                      g_state.tlm.uptime_s, g_state.tlm.mode_change_count);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "  PLL Clock Loss : %-10u   | TMDS Gate Cycles    : %u | Blind Detector Events: %u",
                      g_state.tlm.pll_loss_count, g_state.tlm.gate_cycle_count, g_state.tlm.blind_event_count);
}

static bool s_confirmDefaults = false;
static bool s_confirmEeprom = false;   // the EEPROM write also asks twice

static void render_tab_flash_about(void)
{
    int sel = g_state.selected_item[6];
    int row = 5;

    char uidStr[32];
    snprintf(uidStr, sizeof(uidStr), "%02X%02X%02X%02X-%02X%02X%02X%02X-%02X%02X%02X%02X",
             g_state.about.mcu_uid[0], g_state.about.mcu_uid[1], g_state.about.mcu_uid[2], g_state.about.mcu_uid[3],
             g_state.about.mcu_uid[4], g_state.about.mcu_uid[5], g_state.about.mcu_uid[6], g_state.about.mcu_uid[7],
             g_state.about.mcu_uid[8], g_state.about.mcu_uid[9], g_state.about.mcu_uid[10], g_state.about.mcu_uid[11]);

    // Section 1: Hardware Identity (rows 5-6)
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM,
                      "HARDWARE IDENTITY: FW v%u.%u.%u (SMBus Rev %u) | ADV7511: ID 0x%02X Rev 0x%02X",
                      g_state.fw_ver[0], g_state.fw_ver[1], g_state.fw_ver[2], g_state.fw_ver[3],
                      g_state.about.chip_id, g_state.about.chip_rev);

    Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                      "  MCU UID : %s | Git: %s | Built: %s",
                      uidStr, g_state.about.git[0] ? g_state.about.git : "N/A",
                      g_state.about.built[0] ? g_state.about.built : "N/A");

    // Section 2: Motherboard EEPROM Configuration (rows 7-10)
    const xbox_eeprom_info_t *eeprom = EEPROM_GetInfo();
    Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "XBOX MOTHERBOARD EEPROM VIDEO CONFIGURATION:");

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
                      "  Flags : Cur [480p:%s 720p:%s 1080i:%s 16:9:%s]  Target [480p:%s 720p:%s 1080i:%s 16:9:%s]",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                      (eeprom->current_video_flags & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                      (eeprom->recommended_video_flags & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF");

    // Section 3: Flash Page 63 & System Actions (rows 11-16)
    Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "FLASH PAGE 63 & SYSTEM ACTIONS:");

    if (eeprom->pal_detected) {
        Terminal_PrintRow(row++, (sel == 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                          "%s [ (A)     ] Apply Recommended EEPROM (Switch PAL->NTSC-M & Enable HD)", (sel == 0) ? ">" : " ");
    } else {
        Terminal_PrintRow(row++, (sel == 0) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                          "%s [ (A)     ] Apply Recommended EEPROM Settings (from EDID)", (sel == 0) ? ">" : " ");
    }

    Terminal_PrintRow(row++, (sel == 1) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s [ (X)     ] Save Live Configuration to Flash Page 63", (sel == 1) ? ">" : " ");

    Terminal_PrintRow(row++, (sel == 2) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s [ (Y)     ] Reload Configuration from Flash Page 63", (sel == 2) ? ">" : " ");

    Terminal_PrintRow(row++, (sel == 3) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s [ (START) ] Reapply Current Video Mode Immediately", (sel == 3) ? ">" : " ");

    Terminal_PrintRow(row++, (sel == 4) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s [ (A)     ] Restore Factory Defaults & Save to Flash", (sel == 4) ? ">" : " ");

    Terminal_PrintRow(row++, (sel == 5) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s [ (A)     ] Exit to Dashboard (Reboot / Launch)", (sel == 5) ? ">" : " ");

    if (s_confirmDefaults) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER,
                          "  -> Press (A) again to confirm Restore Factory Defaults, (BACK) to cancel");
    } else if (s_confirmEeprom) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER,
                          "  -> Press (A) again to WRITE the console EEPROM, (BACK) to cancel");
    }
}

// ----------------------------------------------------------------------------
// Action Handlers
// ----------------------------------------------------------------------------

static void handle_action_apply_eeprom(void)
{
    if (!s_confirmEeprom) {
        s_confirmEeprom = true;
        set_status("Press (A) again to CONFIRM: write recommended settings to the console EEPROM");
        return;
    }
    s_confirmEeprom = false;
    const edid_parsed_t *edid = EDID_GetParsed();
    if (EEPROM_ApplyRecommended(edid)) {
        const xbox_eeprom_info_t *info = EEPROM_GetInfo();
        set_status(info->status_str);
    } else {
        const xbox_eeprom_info_t *info = EEPROM_GetInfo();
        set_status(info->status_str[0] ? info->status_str : "ERROR: Failed to update Motherboard EEPROM");
    }
}

static void handle_action_save_flash(void)
{
    if (smbus_trigger_action(XHD_ACTION_SAVE_FLASH)) {
        set_status("SUCCESS: Saved live settings & calibration to Flash Page 63!");
    } else {
        set_status("ERROR: Failed to save to Flash Page 63 (SMBus NACK)");
    }
}

static void handle_action_reload_flash(void)
{
    if (smbus_trigger_action(XHD_ACTION_RELOAD_FLASH)) {
        refresh_all_from_board();
        set_status("SUCCESS: Reloaded saved configuration from Flash Page 63!");
    } else {
        set_status("ERROR: Failed to reload from Flash Page 63");
    }
}

static void handle_action_reapply_mode(void)
{
    if (smbus_trigger_action(XHD_ACTION_REAPPLY_MODE)) {
        set_status("SUCCESS: Reapplied current video mode!");
    } else {
        set_status("ERROR: Reapply mode failed");
    }
}

static void handle_action_restore_defaults(void)
{
    if (!s_confirmDefaults) {
        s_confirmDefaults = true;
        set_status("Press (A) again to CONFIRM: Restore factory defaults to Flash");
        return;
    }
    s_confirmDefaults = false;
    if (smbus_trigger_action(XHD_ACTION_RESTORE_DEFAULTS)) {
        refresh_all_from_board();
        set_status("SUCCESS: Restored factory defaults & saved to Flash!");
    } else {
        set_status("ERROR: Restore factory defaults failed");
    }
}

// ----------------------------------------------------------------------------
// Input Processing
// ----------------------------------------------------------------------------

static void process_input(const input_state_t *in)
{
    if (BlackLevel_IsActive()) {
        BlackLevel_HandleInput(in);
        return;
    }

    // Tab switching via White / Black buttons
    int prev_tab = g_state.active_tab;
    if (in->white) {
        g_state.active_tab = (g_state.active_tab + NUM_TABS - 1) % NUM_TABS;
    }
    if (in->black) {
        g_state.active_tab = (g_state.active_tab + 1) % NUM_TABS;
    }
    if (g_state.active_tab < 0 || g_state.active_tab >= NUM_TABS) {
        g_state.active_tab = 0;
    }
    if (g_state.active_tab != prev_tab) {
        s_confirmDefaults = false;
        s_confirmEeprom = false;
        if (prev_tab == 1) {
            // Restore native 720p mode when leaving Picture Position screen
            int idx720 = VideoMode_FindMatch(1280, 720, false);
            const video_mode_info_t *m720 = VideoMode_Get(idx720);
            if (m720 && (Terminal_GetWidth() != m720->width || Terminal_GetHeight() != m720->height)) {
                if (Terminal_SetVideoMode(m720->width, m720->height, m720->d3d_flags, m720->refresh_rate)) {
                    VideoMode_SetCurrentIndex(idx720);
                }
            }
        }
        if (g_state.active_tab == 0) {
            smbus_read_video_info(&g_state.vi);
        } else if (g_state.active_tab == 4) {
            smbus_read_telemetry(&g_state.tlm);
        } else if (g_state.active_tab == 5) {
            if (!EDID_HasData()) {
                EDID_RefreshFromBoard();
            }
        } else if (g_state.active_tab == 6) {
            EEPROM_Refresh(EDID_GetParsed());
        }
    }

    // Global quick action buttons (do not steal X or Y on Tab 3 CEC Tester, Tab 5 EDID Inspector, or Tab 7 About)
    if (in->x && g_state.active_tab != 3 && g_state.active_tab != 5 && g_state.active_tab != 7) {
        handle_action_save_flash();
        return;
    }
    if (in->y && g_state.active_tab != 1 && g_state.active_tab != 3 && g_state.active_tab != 5 && g_state.active_tab != 7) { // tab 1 Y centers, tab 3 Y clears log, tab 5 Y refreshes EDID
        handle_action_reload_flash();
        return;
    }
    if (in->start && g_state.active_tab != 1 && g_state.active_tab != 3 && g_state.active_tab != 5 && g_state.active_tab != 7) {
        handle_action_reapply_mode();
        return;
    }

    int tab = g_state.active_tab;

    int max_items = 0;
    switch (tab) {
        case 0: max_items = 7; break; // Video Settings
        case 1: max_items = 1; break; // Picture Position
        case 2: max_items = 4; break; // CEC & SPD
        case 3: max_items = 1; break; // CEC Remote Tester
        case 4: max_items = 1; break; // Telemetry
        case 5: max_items = 1; break; // EDID Inspector
        case 6: max_items = 6; break; // Flash & Hardware (0: Apply EEPROM, 1: Save, 2: Reload, 3: Reapply, 4: Restore Defaults, 5: Exit to Dashboard)
        case 7: max_items = 1; break; // About (internal cursor)
    }

    // Up / Down item navigation (bypassed on Tab 1 where D-Pad moves the raster)
    if (tab != 1) {
        if (in->up) {
            if (g_state.selected_item[tab] > 0) {
                g_state.selected_item[tab]--;
                s_confirmDefaults = false;
                s_confirmEeprom = false;
            }
        }
        if (in->down) {
            if (g_state.selected_item[tab] < max_items - 1) {
                g_state.selected_item[tab]++;
                s_confirmDefaults = false;
                s_confirmEeprom = false;
            }
        }
    }

    int sel = g_state.selected_item[tab];

    // Tab 0: Video Settings Modification
    if (tab == 0) {
        if (sel == 0) { // Color Space
            if (in->right || in->a) {
                g_state.color_space = (g_state.color_space + 1) % 6;
                set_result(smbus_set_color_space(g_state.color_space), "Colour Space changed (Live reapply)");
            } else if (in->left) {
                g_state.color_space = (g_state.color_space + 5) % 6;
                set_result(smbus_set_color_space(g_state.color_space), "Colour Space changed (Live reapply)");
            }
        } else if (sel == 1) { // Aspect Ratio
            if (in->right || in->a) {
                g_state.aspect_ratio = (g_state.aspect_ratio + 1) % 3;
                set_result(smbus_set_aspect_ratio(g_state.aspect_ratio), "Aspect Ratio changed (Applies to SD modes)");
            } else if (in->left) {
                g_state.aspect_ratio = (g_state.aspect_ratio + 2) % 3;
                set_result(smbus_set_aspect_ratio(g_state.aspect_ratio), "Aspect Ratio changed (Applies to SD modes)");
            }
        } else if (sel == 2) { // Scan Mode
            if (in->right || in->a) {
                g_state.scan_mode = (g_state.scan_mode + 1) % 3;
                set_result(smbus_set_scan_mode(g_state.scan_mode), "Scan Mode changed (Live reapply)");
            } else if (in->left) {
                g_state.scan_mode = (g_state.scan_mode + 2) % 3;
                set_result(smbus_set_scan_mode(g_state.scan_mode), "Scan Mode changed (Live reapply)");
            }
        } else if (sel == 3) { // Wire VIC Mode
            bool changed = false;
            if (in->right) {
                if (!g_state.wire_vic_mode) { g_state.wire_vic_mode = true; changed = true; }
            } else if (in->left) {
                if (g_state.wire_vic_mode) { g_state.wire_vic_mode = false; changed = true; }
            } else if (in->a) {
                g_state.wire_vic_mode = !g_state.wire_vic_mode;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_wire_vic_mode(g_state.wire_vic_mode), g_state.wire_vic_mode ? "Wire-VIC Mode: 1 (Chip-Auto Detection)" : "Wire-VIC Mode: 0 (Honest CEA Standard)");
            }
        } else if (sel == 4) { // Clock Phase
            if (in->right || in->a) {
                g_state.clock_phase = (g_state.clock_phase + 1) % 8;
                set_result(smbus_set_clock_phase(g_state.clock_phase), "Clock Phase trim adjusted");
            } else if (in->left) {
                g_state.clock_phase = (g_state.clock_phase + 7) % 8;
                set_result(smbus_set_clock_phase(g_state.clock_phase), "Clock Phase trim adjusted");
            }
        } else if (sel == 5) { // Audio Mute
            bool changed = false;
            if (in->right) {
                if (!g_state.audio_mute) { g_state.audio_mute = true; changed = true; }
            } else if (in->left) {
                if (g_state.audio_mute) { g_state.audio_mute = false; changed = true; }
            } else if (in->a) {
                g_state.audio_mute = !g_state.audio_mute;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_audio_mute(g_state.audio_mute), g_state.audio_mute ? "HDMI Audio Muted" : "HDMI Audio Unmuted [Active]");
            }
        } else if (sel == 6) { // Standalone Freeze
            bool changed = false;
            if (in->right) {
                if (!g_state.smbus_freeze) { g_state.smbus_freeze = true; changed = true; }
            } else if (in->left) {
                if (g_state.smbus_freeze) { g_state.smbus_freeze = false; changed = true; }
            } else if (in->a) {
                g_state.smbus_freeze = !g_state.smbus_freeze;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_freeze(g_state.smbus_freeze), g_state.smbus_freeze ? "Standalone Freeze Enabled" : "Standalone Freeze Disabled");
            }
        }
    }
    // Tab 1: Picture Position & Video Mode
    else if (tab == 1) {
        if (in->a) {
            BlackLevel_SetActive(true);
            return;
        }

        // Cycle to next video mode with START
        if (in->start) {
            int curIdx = VideoMode_GetCurrentIndex();
            int nextIdx = VideoMode_GetNextIndex(curIdx);
            const video_mode_info_t *nextMode = VideoMode_Get(nextIdx);
            if (nextMode) {
                if (Terminal_SetVideoMode(nextMode->width, nextMode->height, nextMode->d3d_flags, nextMode->refresh_rate)) {
                    VideoMode_SetCurrentIndex(nextIdx);
                    Sleep(200); // Allow ADV7511 line detector & clock to settle
                    smbus_read_video_info(&g_state.vi);
                    g_state.pos_h = g_state.vi.pos_h;
                    g_state.pos_v = g_state.vi.pos_v;
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Switched mode: %s", nextMode->name);
                    set_status(msg);
                } else {
                    set_status("Failed to switch video mode (D3D reset error)");
                }
            }
            return;
        }

        // Cycle to previous video mode with BACK
        if (in->back) {
            int curIdx = VideoMode_GetCurrentIndex();
            int prevIdx = VideoMode_GetPrevIndex(curIdx);
            const video_mode_info_t *prevMode = VideoMode_Get(prevIdx);
            if (prevMode) {
                if (Terminal_SetVideoMode(prevMode->width, prevMode->height, prevMode->d3d_flags, prevMode->refresh_rate)) {
                    VideoMode_SetCurrentIndex(prevIdx);
                    Sleep(200);
                    smbus_read_video_info(&g_state.vi);
                    g_state.pos_h = g_state.vi.pos_h;
                    g_state.pos_v = g_state.vi.pos_v;
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Switched mode: %s", prevMode->name);
                    set_status(msg);
                } else {
                    set_status("Failed to switch video mode (D3D reset error)");
                }
            }
            return;
        }

        if (in->y) {
            g_state.pos_h = 0;
            g_state.pos_v = 0;
            bool ok = smbus_set_position_h(0);
            ok = smbus_set_position_v(0) && ok;
            set_result(ok, "Position reset to default (0, 0)");
            return;
        }

        int delta_h = 0;
        int delta_v = 0;

        if (in->left) delta_h -= 1;
        if (in->right) delta_h += 1;
        if (in->up) delta_v -= 1;
        if (in->down) delta_v += 1;

        if (in->l_trigger) delta_h -= 10;
        if (in->r_trigger) delta_h += 10;

        if (delta_h != 0) {
            int new_h = g_state.pos_h + delta_h;
            if (new_h < -127) new_h = -127;
            if (new_h > 127) new_h = 127;
            g_state.pos_h = (int8_t)new_h;
            set_result(smbus_set_position_h(g_state.pos_h), "Live horizontal position updated");
        }

        if (delta_v != 0) {
            int new_v = g_state.pos_v + delta_v;
            if (new_v < -127) new_v = -127;
            if (new_v > 127) new_v = 127;
            g_state.pos_v = (int8_t)new_v;
            set_result(smbus_set_position_v(g_state.pos_v), "Live vertical position updated");
        }
    }
    // Tab 2: CEC & SPD
    else if (tab == 2) {
        if (sel == 0) {
            bool changed = false;
            if (in->right) {
                if (!g_state.cec_clock) { g_state.cec_clock = true; changed = true; }
            } else if (in->left) {
                if (g_state.cec_clock) { g_state.cec_clock = false; changed = true; }
            } else if (in->a) {
                g_state.cec_clock = !g_state.cec_clock;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_cec_clock(g_state.cec_clock), g_state.cec_clock ? "CEC 12MHz Clock Enabled" : "CEC 12MHz Clock Disabled");
            }
        } else if (sel == 1) {
            bool changed = false;
            if (in->right) {
                if (!g_state.cec_one_touch) { g_state.cec_one_touch = true; changed = true; }
            } else if (in->left) {
                if (g_state.cec_one_touch) { g_state.cec_one_touch = false; changed = true; }
            } else if (in->a) {
                g_state.cec_one_touch = !g_state.cec_one_touch;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_cec_one_touch(g_state.cec_one_touch), g_state.cec_one_touch ? "CEC One Touch Play Enabled" : "CEC One Touch Play Disabled");
            }
        } else if (sel == 2) {
            bool changed = false;
            if (in->right) {
                if (!g_state.cec_standby) { g_state.cec_standby = true; changed = true; }
            } else if (in->left) {
                if (g_state.cec_standby) { g_state.cec_standby = false; changed = true; }
            } else if (in->a) {
                g_state.cec_standby = !g_state.cec_standby;
                changed = true;
            }
            if (changed) {
                set_result(smbus_set_cec_standby(g_state.cec_standby), g_state.cec_standby ? "CEC Shutdown Standby Enabled" : "CEC Shutdown Standby Disabled");
            }
        } else if (sel == 3) {
            if (in->a) {
                OSK_Open(g_state.spd_name);
            } else if (in->right || in->left) {
                int totalOptions = NUM_SPD_PRESETS + (s_hasCustomSpd ? 1 : 0);
                int curIdx = -1;

                if (s_hasCustomSpd && strncmp(g_state.spd_name, s_customSpd, 24) == 0) {
                    curIdx = 0;
                } else {
                    for (int i = 0; i < NUM_SPD_PRESETS; i++) {
                        if (strncmp(g_state.spd_name, kSpdPresets[i], 24) == 0) {
                            curIdx = s_hasCustomSpd ? (i + 1) : i;
                            break;
                        }
                    }
                }

                if (curIdx < 0) {
                    strncpy(s_customSpd, g_state.spd_name, sizeof(s_customSpd) - 1);
                    s_customSpd[sizeof(s_customSpd) - 1] = '\0';
                    s_hasCustomSpd = true;
                    totalOptions = NUM_SPD_PRESETS + 1;
                    curIdx = 0;
                }

                if (in->right) {
                    curIdx = (curIdx + 1) % totalOptions;
                } else if (in->left) {
                    curIdx = (curIdx + totalOptions - 1) % totalOptions;
                }

                if (s_hasCustomSpd && curIdx == 0) {
                    strncpy(g_state.spd_name, s_customSpd, 24);
                    g_state.spd_name[24] = '\0';
                    set_result(smbus_write_spd(g_state.spd_name), "SPD TV Label restored to custom entry");
                } else {
                    int pIdx = s_hasCustomSpd ? (curIdx - 1) : curIdx;
                    strncpy(g_state.spd_name, kSpdPresets[pIdx], 24);
                    g_state.spd_name[24] = '\0';
                    set_result(smbus_write_spd(g_state.spd_name), "SPD TV Input Name Preset committed");
                }
            }
        }
    }
    // Tab 3: CEC Remote Tester
    else if (tab == 3) {
        CECRemote_HandleInput(in);
    }
    // Tab 5: EDID Inspector
    else if (tab == 5) {
        EDID_HandleInput(in);
    }
    // Tab 6: Flash & About
    else if (tab == 6) {
        if (in->a) {
            switch (sel) {
                case 0: handle_action_apply_eeprom(); break;
                case 1: handle_action_save_flash(); break;
                case 2: handle_action_reload_flash(); break;
                case 3: handle_action_reapply_mode(); break;
                case 4: handle_action_restore_defaults(); break;
                case 5: UI_RunCRTCollapseAndExit(); break;
            }
        }
        if (in->back) {
            if (s_confirmDefaults || s_confirmEeprom) {
                s_confirmDefaults = false;
                s_confirmEeprom = false;
                set_status("Action cancelled");
            }
        }
    }
    // Tab 7: About
    else if (tab == 7) {
        About_HandleInput(in);
    }
}

// ----------------------------------------------------------------------------
// Main Application Entry Point
// ----------------------------------------------------------------------------

int main(void)
{
    memset(&g_state, 0, sizeof(app_state_t));
    g_state.active_tab = 0;

    About_Init();
    CECRemote_Init();
    EEPROM_Init();
    BlackLevel_Init();

    // Initialize Xbox controller and Direct3D terminal
    input_init();

    if (!Terminal_Init()) {
        for (;;) {}
    }

    VideoMode_Init();

    // Connect to X-HD over SMBus and read initial state
    refresh_all_from_board();

    // Initial frame render (fill both back and front buffers)
    s_dirty_frames = 2;

    // Reactive Render-on-Demand Loop
    for (;;) {
        DWORD now = GetTickCount();

        // 1. Poll Xbox Controller Input
        input_state_t in;
        input_poll(&in);
        if (in.exit_requested) {
            UI_RunCRTCollapseAndExit();
            break;
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
        }

        // 2. Poll CEC Remote Controller (Tab 3)
        if (CECRemote_Update(g_state.active_tab == 3)) {
            s_dirty_frames = 2;
        }

        // 3. Process Input
        if (OSK_IsActive()) {
            bool committed = false;
            char newSpd[25];
            if (OSK_HandleInput(&in, &committed, newSpd)) {
                s_dirty_frames = 2;
                if (committed) {
                    memcpy(g_state.spd_name, newSpd, 25);
                    memcpy(s_customSpd, newSpd, 25);
                    s_hasCustomSpd = true;
                    if (smbus_write_spd(g_state.spd_name)) {
                        set_status("SPD TV Label applied to board! Save to Flash in Tab 6.");
                    } else {
                        set_status("SMBus Write SPD failed (board busy)");
                    }
                }
            }
        } else {
            process_input(&in);
        }

        // 4. Poll Live Telemetry (Tab 4, 1 Hz)
        if (g_state.active_tab == 4) {
            if (now - g_state.last_poll_tick >= 1000) {
                g_state.last_poll_tick = now;
                smbus_read_telemetry(&g_state.tlm);
                s_dirty_frames = 2;
            }
        }

        // 5. Retro Phosphor Cursor Blink Phase Flip (~2 Hz: 450ms toggle)
        if (!BlackLevel_IsActive()) {
            static bool s_last_blink_phase = false;
            bool cur_blink_phase = ((now / 450) % 2 == 0);
            if (cur_blink_phase != s_last_blink_phase) {
                s_last_blink_phase = cur_blink_phase;
                s_dirty_frames = 2;
            }
        }

        // 6. Status Message Timeout (3.0 seconds)
        if (g_state.status_msg[0] != '\0' && s_status_expire_tick != 0 && now >= s_status_expire_tick) {
            g_state.status_msg[0] = '\0';
            s_status_expire_tick = 0;
            s_dirty_frames = 2;
        }

        // 7. Render on Demand
        if (s_dirty_frames > 0) {
            Terminal_BeginFrame();

            if (OSK_IsActive()) {
                OSK_Render();
            } else if (BlackLevel_IsActive()) {
                BlackLevel_Render();
            } else {
                Terminal_DrawHeader(kTabNames[g_state.active_tab], g_state.fw_ver_str);

                switch (g_state.active_tab) {
                    case 0: render_tab_video(); break;
                    case 1: render_tab_position(); break;
                    case 2: render_tab_cec_spd(); break;
                    case 3: CECRemote_Render(); break;
                    case 4: render_tab_telemetry(); break;
                    case 5: EDID_Render(); break;
                    case 6: render_tab_flash_about(); break;
                    case 7: About_Render(); break;
                }

                // Context-sensitive footer shortcuts
                const char *shortcuts = "D-PAD: Navigate/Adjust | (X): Save Flash | (START): Reapply | Hold (BACK): Exit";
                const char *shortcuts2 = NULL;

                if (Terminal_GetWidth() <= 640) {
                    if (g_state.active_tab == 1) {
                        shortcuts = "[START/BACK]: Mode | [D-PAD]: +/-1 | (A): Black Level | (Y): 0,0";
                        shortcuts2 = "[TRIGGERS]: +/-10 px | (X): Save";
                    } else if (g_state.active_tab == 2) {
                        shortcuts = "(A): Edit SPD | (L/R): Preset | (X): Save Flash";
                    } else if (g_state.active_tab == 3) {
                        shortcuts = "(Y): Clear Log | Press buttons on TV remote";
                    } else if (g_state.active_tab == 5) {
                        shortcuts = "(X): Hex Dump | (Y): Refresh | (D-PAD L/R): Block";
                    } else if (g_state.active_tab == 6) {
                        shortcuts = "(A): Exec Selected | (X): Save | Hold (BACK): Exit";
                    } else if (g_state.active_tab == 7) {
                        shortcuts = "(A): Toggle | (X): Expand/Collapse | TRIGGERS: Scroll";
                    } else {
                        shortcuts = "D-PAD: Adjust | (X): Save | (START): Apply | Hold BACK: Exit";
                    }
                } else {
                    if (g_state.active_tab == 1) {
                        shortcuts = "[START/BACK]: Next/Prev Mode | [D-PAD]: Fine (+/-1) | (A): Black Level Verifier | (Y): Reset";
                        shortcuts2 = "[TRIGGERS]: Coarse Adjust (+/-10 px) | (X): Save Flash";
                    } else if (g_state.active_tab == 2) {
                        shortcuts = "(A): Edit SPD (OSK) | (D-PAD L/R): Preset | (X): Save Flash";
                    } else if (g_state.active_tab == 3) {
                        shortcuts = "(Y): Clear Event Log | Press buttons on TV remote to test";
                    } else if (g_state.active_tab == 5) {
                        shortcuts = "(X): Toggle Hex Dump | (Y): Refresh Sink EDID | (D-PAD L/R): Block";
                    } else if (g_state.active_tab == 6) {
                        shortcuts = "(A): Execute Selected | (X): Save Flash | (Y): Reload | Hold (BACK): Exit";
                    } else if (g_state.active_tab == 7) {
                        shortcuts = "(A): Toggle Section | (X): Expand/Collapse All | TRIGGERS: Scroll";
                    }
                }

                Terminal_DrawFooter(shortcuts, shortcuts2, g_state.status_msg);
            }

            Terminal_EndFrame();
            s_dirty_frames--;
            g_state.activity_frame++;
        } else {
            // Idle: Yield CPU to maintain ~100 Hz input polling responsiveness without spinning
            Sleep(10);
        }
    }

    return 0;
}
