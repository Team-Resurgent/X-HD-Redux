#include "ui_osk.h"
#include "ui_terminal.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define NUM_GRID_ROWS 6

typedef struct {
    const char *vendor;
    const char *product;
} osk_preset_t;

static const osk_preset_t kPresets[] = {
    { "MSFT",     "Xbox" },
    { "MSFT",     "Microsoft Xbox" },
    { "MSFT",     "X-HD" },
    { "MSFT",     "Retro Consoles" },
    { "HALO",     "Combat Evolved" },
    { "SEGA",     "Dreamcast" },
    { "SONY",     "PlayStation" },
    { "NINTENDO", "GameCube" }
};
#define NUM_PRESETS (int)(sizeof(kPresets) / sizeof(kPresets[0]))

static const char kRow0[] = "1234567890-_";
static const char kRow1U[] = "QWERTYUIOP/:";
static const char kRow1L[] = "qwertyuiop/:";
static const char kRow2U[] = "ASDFGHJKL.,'";
static const char kRow2L[] = "asdfghjkl.,'";
static const char kRow3U[] = "ZXCVBNM()+!?";
static const char kRow3L[] = "zxcvbnm()+!?";

static bool s_active = false;
static int  s_activeField = 1; // 0 = Vendor, 1 = Product (default to product)
static char s_vendor[9];       // 8 chars + null
static char s_product[17];     // 16 chars + null

static int  s_gridRow = 1;     // Start on QWERTY letter row
static int  s_gridCol = 0;
static bool s_caps = true;     // Default uppercase
static int  s_presetIndex = 0;
static int  s_blinkCounter = 0;

static void trim_trailing_spaces(char *str)
{
    int len = (int)strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[len - 1] = '\0';
        len--;
    }
}

void OSK_Open(const char currentSpd[25])
{
    memset(s_vendor, 0, sizeof(s_vendor));
    memset(s_product, 0, sizeof(s_product));

    if (currentSpd) {
        int inLen = (int)strlen(currentSpd);
        // Vendor: bytes 0..7
        for (int i = 0; i < 8; i++) {
            char c = (i < inLen) ? currentSpd[i] : ' ';
            s_vendor[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
        }
        s_vendor[8] = '\0';
        trim_trailing_spaces(s_vendor);

        // Product: bytes 8..23
        for (int i = 0; i < 16; i++) {
            char c = (8 + i < inLen) ? currentSpd[8 + i] : ' ';
            s_product[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
        }
        s_product[16] = '\0';
        trim_trailing_spaces(s_product);
    } else {
        strncpy(s_vendor, "MSFT", sizeof(s_vendor) - 1);
        strncpy(s_product, "Xbox", sizeof(s_product) - 1);
    }

    s_active = true;
    s_activeField = 1; // Default to editing Product
    s_gridRow = 1;
    s_gridCol = 0;
    s_caps = true;
    s_blinkCounter = 0;
}

bool OSK_IsActive(void)
{
    return s_active;
}

static void apply_preset(int index)
{
    if (index < 0 || index >= NUM_PRESETS) return;
    strncpy(s_vendor, kPresets[index].vendor, sizeof(s_vendor) - 1);
    s_vendor[sizeof(s_vendor) - 1] = '\0';
    strncpy(s_product, kPresets[index].product, sizeof(s_product) - 1);
    s_product[sizeof(s_product) - 1] = '\0';
}

static void cycle_preset(void)
{
    s_presetIndex = (s_presetIndex + 1) % NUM_PRESETS;
    apply_preset(s_presetIndex);
}

static void append_char(char c)
{
    char *target = (s_activeField == 0) ? s_vendor : s_product;
    int maxLen = (s_activeField == 0) ? 8 : 16;
    int len = (int)strlen(target);

    if (len < maxLen) {
        target[len] = c;
        target[len + 1] = '\0';
    }
}

static void backspace_char(void)
{
    char *target = (s_activeField == 0) ? s_vendor : s_product;
    int len = (int)strlen(target);
    if (len > 0) {
        target[len - 1] = '\0';
    }
}

static void clear_field(void)
{
    char *target = (s_activeField == 0) ? s_vendor : s_product;
    target[0] = '\0';
}

static void commit_buffer(char outSpd[25])
{
    memset(outSpd, ' ', 24);

    int vlen = (int)strlen(s_vendor);
    if (vlen > 8) vlen = 8;
    memcpy(outSpd, s_vendor, (size_t)vlen);

    int plen = (int)strlen(s_product);
    if (plen > 16) plen = 16;
    memcpy(outSpd + 8, s_product, (size_t)plen);

    outSpd[24] = '\0';
}

bool OSK_HandleInput(const input_state_t *in, bool *outCommitted, char outSpd[25])
{
    if (!s_active || !in) return false;

    // Quick direct controller shortcuts
    if (in->white) {
        s_activeField = 0; // Switch to Vendor
        return false;
    }
    if (in->black) {
        s_activeField = 1; // Switch to Product
        return false;
    }
    if (in->x) {
        backspace_char();
        return false;
    }
    if (in->y) {
        append_char(' ');
        return false;
    }
    if (in->l_trigger) {
        s_caps = !s_caps;
        return false;
    }
    if (in->r_trigger) {
        cycle_preset();
        return false;
    }
    if (in->b) {
        // Cancel without saving
        s_active = false;
        if (outCommitted) *outCommitted = false;
        return true;
    }
    if (in->start) {
        // Save and commit immediately
        commit_buffer(outSpd);
        s_active = false;
        if (outCommitted) *outCommitted = true;
        return true;
    }

    // Grid Navigation
    if (in->left) {
        s_gridCol--;
        if (s_gridCol < 0) {
            if (s_gridRow < 4) s_gridCol = 11;
            else if (s_gridRow == 4) s_gridCol = 4;
            else s_gridCol = 1;
        }
    }
    if (in->right) {
        s_gridCol++;
        int maxCol = (s_gridRow < 4) ? 11 : ((s_gridRow == 4) ? 4 : 1);
        if (s_gridCol > maxCol) {
            s_gridCol = 0;
        }
    }
    if (in->down) {
        s_gridRow++;
        if (s_gridRow >= NUM_GRID_ROWS) {
            s_gridRow = 0;
        }
        // Column mapping when moving between different width rows
        if (s_gridRow == 4) {
            s_gridCol = (s_gridCol * 5) / 12;
            if (s_gridCol > 4) s_gridCol = 4;
        } else if (s_gridRow == 5) {
            s_gridCol = (s_gridCol >= 3) ? 1 : 0;
        } else if (s_gridRow == 0) {
            s_gridCol = (s_gridCol == 0) ? 3 : 9;
        }
    }
    if (in->up) {
        s_gridRow--;
        if (s_gridRow < 0) {
            s_gridRow = NUM_GRID_ROWS - 1;
        }
        if (s_gridRow == 4) {
            s_gridCol = (s_gridCol == 0) ? 1 : 4;
        } else if (s_gridRow == 3) {
            s_gridCol = (s_gridCol * 12) / 5;
            if (s_gridCol > 11) s_gridCol = 11;
        } else if (s_gridRow == 5) {
            s_gridCol = (s_gridCol < 6) ? 0 : 1;
        }
    }

    // Action execution on (A)
    if (in->a) {
        if (s_gridRow == 0) {
            append_char(kRow0[s_gridCol]);
        } else if (s_gridRow == 1) {
            append_char(s_caps ? kRow1U[s_gridCol] : kRow1L[s_gridCol]);
        } else if (s_gridRow == 2) {
            append_char(s_caps ? kRow2U[s_gridCol] : kRow2L[s_gridCol]);
        } else if (s_gridRow == 3) {
            append_char(s_caps ? kRow3U[s_gridCol] : kRow3L[s_gridCol]);
        } else if (s_gridRow == 4) {
            switch (s_gridCol) {
                case 0: append_char(' '); break;
                case 1: backspace_char(); break;
                case 2: clear_field(); break;
                case 3: s_caps = !s_caps; break;
                case 4: cycle_preset(); break;
            }
        } else if (s_gridRow == 5) {
            if (s_gridCol == 0) {
                commit_buffer(outSpd);
                s_active = false;
                if (outCommitted) *outCommitted = true;
                return true;
            } else {
                s_active = false;
                if (outCommitted) *outCommitted = false;
                return true;
            }
        }
    }

    return false;
}

void OSK_Render(void)
{
    if (!s_active) return;

    bool blink = ((GetTickCount() / 450) % 2 == 0);

    // Header
    Terminal_DrawHLine(0, '=', COLOR_PHOSPHOR_ACCENT);
    Terminal_PrintRowCenter(1, COLOR_PHOSPHOR_BRIGHT, "=== HDMI SPD TV INPUT LABEL EDITOR ===");
    Terminal_DrawHLine(2, '=', COLOR_PHOSPHOR_ACCENT);

    // Vendor Field (Row 4)
    char vDisp[32];
    if (s_activeField == 0 && blink && strlen(s_vendor) < 8) {
        snprintf(vDisp, sizeof(vDisp), "%s_", s_vendor);
    } else {
        snprintf(vDisp, sizeof(vDisp), "%s", s_vendor);
    }

    if (s_activeField == 0) {
        Terminal_PrintRow(4, COLOR_PHOSPHOR_BRIGHT, "> VENDOR  (8B) : [ %-8s ]  <-- EDITING (Press BLACK for Product)", vDisp);
    } else {
        Terminal_PrintRow(4, COLOR_PHOSPHOR_NORMAL, "  VENDOR  (8B) : [ %-8s ]      (Press WHITE to Edit)", vDisp);
    }

    // Product Field (Row 5)
    char pDisp[32];
    if (s_activeField == 1 && blink && strlen(s_product) < 16) {
        snprintf(pDisp, sizeof(pDisp), "%s_", s_product);
    } else {
        snprintf(pDisp, sizeof(pDisp), "%s", s_product);
    }

    if (s_activeField == 1) {
        Terminal_PrintRow(5, COLOR_PHOSPHOR_BRIGHT, "> PRODUCT (16B): [ %-16s ]  <-- EDITING (Press WHITE for Vendor)", pDisp);
    } else {
        Terminal_PrintRow(5, COLOR_PHOSPHOR_NORMAL, "  PRODUCT (16B): [ %-16s ]      (Press BLACK to Edit)", pDisp);
    }

    // Combined live preview (Row 7)
    char combined[25];
    commit_buffer(combined);

    char cecOsd[15];
    memset(cecOsd, ' ', 14);
    int plen = (int)strlen(s_product);
    if (plen > 14) plen = 14;
    memcpy(cecOsd, s_product, (size_t)plen);
    cecOsd[14] = '\0';

    Terminal_PrintRow(7, COLOR_PHOSPHOR_DIM, "  Live 24B SPD: \"%s\"   |   CEC OSD Name: \"%s\"", combined, cecOsd);
    Terminal_DrawHLine(8, '-', COLOR_PHOSPHOR_DARK);

    // Keyboard Key Grids
    int startX = (Terminal_GetWidth() - (12 * 76)) / 2;
    int lineHeight = Terminal_GetLineHeight();
    int topMargin = Terminal_GetTopMargin();

    // Helper macro for rendering keys
    #define DRAW_ROW_KEYS(rowIdx, screenRow, strChars) do { \
        int y = topMargin + (screenRow) * lineHeight; \
        for (int c = 0; c < 12; c++) { \
            int x = startX + c * 76; \
            bool isSel = (s_gridRow == (rowIdx) && s_gridCol == c); \
            DWORD col = isSel ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL; \
            if (isSel) { \
                Terminal_PrintAt(x, y, col, ">%c<", (strChars)[c]); \
            } else { \
                Terminal_PrintAt(x, y, col, "[%c]", (strChars)[c]); \
            } \
        } \
    } while (0)

    // Row 0 (screen row 9)
    DRAW_ROW_KEYS(0, 9, kRow0);

    // Row 1 (screen row 11)
    DRAW_ROW_KEYS(1, 11, s_caps ? kRow1U : kRow1L);

    // Row 2 (screen row 13)
    DRAW_ROW_KEYS(2, 13, s_caps ? kRow2U : kRow2L);

    // Row 3 (screen row 15)
    DRAW_ROW_KEYS(3, 15, s_caps ? kRow3U : kRow3L);

    #undef DRAW_ROW_KEYS

    // Row 4: Action Keys (screen row 17)
    {
        int y = topMargin + 17 * lineHeight;
        const char *btnLabels[5];
        btnLabels[0] = "[ SPACE ]";
        btnLabels[1] = "[ BKSP ]";
        btnLabels[2] = "[ CLEAR ]";
        btnLabels[3] = s_caps ? "[ CAPS: ON ]" : "[ CAPS: OFF ]";
        btnLabels[4] = "[ PRESET ]";

        const char *selLabels[5];
        selLabels[0] = "> SPACE <";
        selLabels[1] = "> BKSP <";
        selLabels[2] = "> CLEAR <";
        selLabels[3] = s_caps ? "> CAPS: ON <" : "> CAPS: OFF <";
        selLabels[4] = "> PRESET <";

        for (int c = 0; c < 5; c++) {
            int x = startX + c * 182;
            bool isSel = (s_gridRow == 4 && s_gridCol == c);
            DWORD col = isSel ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL;
            Terminal_PrintAt(x, y, col, "%s", isSel ? selLabels[c] : btnLabels[c]);
        }
    }

    // Row 5: Commit & Cancel (screen row 19)
    {
        int y = topMargin + 19 * lineHeight;
        int x0 = startX + 60;
        int x1 = startX + 500;

        bool sel0 = (s_gridRow == 5 && s_gridCol == 0);
        bool sel1 = (s_gridRow == 5 && s_gridCol == 1);

        Terminal_PrintAt(x0, y, sel0 ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                         "%s", sel0 ? ">> [ SAVE & APPLY (START) ] <<" : "[ SAVE & APPLY (START) ]");

        Terminal_PrintAt(x1, y, sel1 ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                         "%s", sel1 ? ">> [ CANCEL (B) ] <<" : "[ CANCEL (B) ]");
    }

    // Bottom Separator & Shortcuts (Rows 20..22)
    Terminal_DrawHLine(20, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(21, COLOR_PHOSPHOR_AMBER,
                      "KEYS: (A) Type  |  (X) Backspace  |  (Y) Space  |  (LT) Caps  |  (RT) Next Preset");
    Terminal_PrintRow(22, COLOR_PHOSPHOR_BRIGHT,
                      "SAVE: (START) Save & Apply to Board   |   EXIT: (B) Cancel   |   FIELD: (WHITE/BLACK)");
}
