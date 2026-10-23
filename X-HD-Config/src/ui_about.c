#include "ui_about.h"
#include "ui_terminal.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *title;
    int         num_lines;
    const char *lines[8];
} about_section_t;

#define NUM_SECTIONS 7

static const about_section_t kSections[NUM_SECTIONS] = {
    {
        "PAGE 1: VIDEO & ENCODER SETTINGS",
        7,
        {
            "Colour Space   : Auto (EDID match), RGB Full/Limited, or YCbCr 4:4:4 direct.",
            "Aspect Ratio   : Sets 4:3 standard or 16:9 widescreen anamorphic flag for SD.",
            "Scan Mode      : Auto (Game Mode), TV (overscan), PC (1:1 pixel mapping).",
            "Wire-VIC Mode  : 0 = Strict CEA-861 standard, 1 = ADV7511 auto detection.",
            "Clock Phase    : 8-step fine trim (0.4ns) of the ADV7511 input clock vs. data.",
            "Audio Output   : Enables or mutes digital audio over HDMI TMDS stream.",
            "Standalone Frz : Freezes encoder registers to isolate and test video modes."
        }
    },
    {
        "PAGE 2: PICTURE ALIGNMENT & RETICLE",
        4,
        {
            "Mode Switching : Cycle START/BACK between 720p, 1080i, 480p, 480i to calibrate profiles.",
            "Horizontal (HS): Fine-tunes active pixel start position (-127 to +127 px).",
            "Vertical   (VS): Fine-tunes raster line start position (-127 to +127 lines).",
            "Center Reticle : Live crosshair and boundary box to align picture on screen."
        }
    },
    {
        "PAGE 3: CEC & SPD INFOFRAME CONFIG",
        4,
        {
            "CEC 12MHz Clock: Drives optional 12MHz clock output on STM32 pin PA8.",
            "One Touch Play : Auto-wakes TV and switches to Xbox input on console power-on.",
            "CEC Standby    : Sends standby broadcast to shut off TV on console power-down.",
            "HDMI SPD Label : Custom 24-character TV input label with preset & OSK support."
        }
    },
    {
        "PAGE 4: CEC REMOTE TESTER",
        4,
        {
            "Visual Remote  : Live on-screen remote control highlighting pressed buttons.",
            "D-Pad & Enter  : Validates Up, Down, Left, Right & Select/OK remote codes.",
            "Universal Keys : Tests Power, Info, Back, Menu and Play/Pause controls.",
            "Dynamic Badge  : Real-time decoder badge for color keys, numbers & media."
        }
    },
    {
        "PAGE 5: LIVE TELEMETRY & HARDWARE",
        4,
        {
            "Pixel Clock    : Real-time measured clock frequency & ADV7511 PLL lock state.",
            "Arbiter VIC    : Target video mode resolved from line detector vs BIOS packet.",
            "Hardware Flags : Live Hot-Plug (HPD), Monitor Sense, TMDS gating & Audio state.",
            "Lifetime Stats : Board uptime, applied mode changes & TMDS gate cycles."
        }
    },
    {
        "PAGE 6: EDID SINK INSPECTOR",
        3,
        {
            "Decoded Summary: Connected display model, native resolution, VICs & audio.",
            "Raw Hex Dump   : 256-byte dual-block hex/ASCII viewer for Block 0 & Block 1.",
            "Controls       : (X) toggles Decoded/Hex view; (Y) re-reads fresh EDID from sink."
        }
    },
    {
        "PAGE 7: FLASH PAGE 63 & ACTIONS",
        6,
        {
            "Hardware ID    : Firmware version, build date, Git SHA, 96-bit MCU UID.",
            "Apply EEPROM   : Writes the console's HD/aspect flags recommended by the EDID.",
            "Save to Flash  : Commits all live settings permanently to onboard Page 63.",
            "Reload Config  : Restores settings back to last saved flash snapshot.",
            "Reapply Mode   : Re-syncs ADV7511 encoder pipeline to current video mode.",
            "Restore Default: Clears flash page 63 and restores factory defaults."
        }
    }
};

static bool s_expanded[NUM_SECTIONS] = { true, false, false, false, false, false, false };
static int  s_cursor = 0;       // 0 .. NUM_SECTIONS - 1
static int  s_scrollOffset = 0; // vertical scroll offset in lines

void About_Init(void)
{
    s_expanded[0] = true;
    for (int i = 1; i < NUM_SECTIONS; i++) {
        s_expanded[i] = false;
    }
    s_cursor = 0;
    s_scrollOffset = 0;
}

void About_HandleInput(const input_state_t *in)
{
    if (!in) return;

    if (in->up) {
        if (s_cursor > 0) s_cursor--;
    }
    if (in->down) {
        if (s_cursor < NUM_SECTIONS - 1) s_cursor++;
    }
    if (in->a) {
        s_expanded[s_cursor] = !s_expanded[s_cursor];
    }
    if (in->right) {
        s_expanded[s_cursor] = true;
    }
    if (in->left) {
        s_expanded[s_cursor] = false;
    }
    if (in->x) {
        // Toggle Expand All / Collapse All
        bool any_collapsed = false;
        for (int i = 0; i < NUM_SECTIONS; i++) {
            if (!s_expanded[i]) {
                any_collapsed = true;
                break;
            }
        }
        for (int i = 0; i < NUM_SECTIONS; i++) {
            s_expanded[i] = any_collapsed;
        }
    }

    // Smooth scroll controls
    if (in->l_trigger) {
        s_scrollOffset -= 2;
    }
    if (in->r_trigger) {
        s_scrollOffset += 2;
    }
    if (in->thumb_ly > 16000) {
        s_scrollOffset--;
    }
    if (in->thumb_ly < -16000) {
        s_scrollOffset++;
    }
}

typedef struct {
    int   section_idx;
    bool  is_header;
    char  text[128];
    DWORD color;
} render_line_t;

void About_Render(void)
{
    render_line_t lines[64];
    int num_lines = 0;
    int header_line_idx[NUM_SECTIONS];

    for (int s = 0; s < NUM_SECTIONS; s++) {
        if (num_lines < 64) {
            header_line_idx[s] = num_lines;
            lines[num_lines].section_idx = s;
            lines[num_lines].is_header = true;
            lines[num_lines].color = (s == s_cursor) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL;
            snprintf(lines[num_lines].text, sizeof(lines[num_lines].text),
                     "%s [%c] %s",
                     (s == s_cursor) ? ">" : " ",
                     s_expanded[s] ? '-' : '+',
                     kSections[s].title);
            num_lines++;
        }

        if (s_expanded[s]) {
            for (int l = 0; l < kSections[s].num_lines && num_lines < 64; l++) {
                lines[num_lines].section_idx = s;
                lines[num_lines].is_header = false;
                lines[num_lines].color = COLOR_PHOSPHOR_NORMAL;
                snprintf(lines[num_lines].text, sizeof(lines[num_lines].text),
                         "     * %s", kSections[s].lines[l]);
                num_lines++;
            }
            // Spacer row between expanded sections
            if (num_lines < 64) {
                lines[num_lines].section_idx = s;
                lines[num_lines].is_header = false;
                lines[num_lines].color = COLOR_PHOSPHOR_DIM;
                lines[num_lines].text[0] = '\0';
                num_lines++;
            }
        }
    }

    // Ensure highlighted header stays in the visible window
    if (header_line_idx[s_cursor] < s_scrollOffset) {
        s_scrollOffset = header_line_idx[s_cursor];
    } else if (header_line_idx[s_cursor] >= s_scrollOffset + 13) {
        s_scrollOffset = header_line_idx[s_cursor] - 12;
    }

    int max_scroll = (num_lines > 13) ? (num_lines - 13) : 0;
    if (s_scrollOffset > max_scroll) s_scrollOffset = max_scroll;
    if (s_scrollOffset < 0) s_scrollOffset = 0;

    // Scroll status indicator on top right
    if (max_scroll > 0) {
        Terminal_PrintRowRight(5, COLOR_PHOSPHOR_DIM,
                               "[ SCROLL: LT / RT (Line %d of %d) ]",
                               s_scrollOffset + 1, num_lines);
    }

    // Render visible rows (rows 6 to 18)
    for (int r = 0; r < 13; r++) {
        int lIdx = s_scrollOffset + r;
        if (lIdx < num_lines) {
            if (lines[lIdx].text[0] != '\0') {
                Terminal_PrintRow(6 + r, lines[lIdx].color, "%s", lines[lIdx].text);
            }
        }
    }
}
