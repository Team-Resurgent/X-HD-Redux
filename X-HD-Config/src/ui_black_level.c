#include "ui_black_level.h"
#include "ui_terminal.h"
#include <stdio.h>
#include <string.h>

static bool s_active = false;
static black_level_range_t s_range = BLACK_LEVEL_RANGE_LIMITED;

void BlackLevel_Init(void)
{
    s_active = false;
    s_range = BLACK_LEVEL_RANGE_LIMITED;
}

bool BlackLevel_IsActive(void)
{
    return s_active;
}

void BlackLevel_SetActive(bool active)
{
    s_active = active;
}

void BlackLevel_HandleInput(const input_state_t *in)
{
    if (!in) return;

    if (in->x) {
        s_range = (s_range == BLACK_LEVEL_RANGE_LIMITED) ? BLACK_LEVEL_RANGE_FULL : BLACK_LEVEL_RANGE_LIMITED;
    }

    if (in->b || in->back) {
        s_active = false;
    }
}

void BlackLevel_Render(void)
{
    float sw = (float)Terminal_GetWidth();
    float sh = (float)Terminal_GetHeight();
    int lh = Terminal_GetLineHeight();
    int topM = Terminal_GetTopMargin();

    // 1. Clear background to Reference Black
    // In Limited mode: Code 16 is reference black.
    // In Full mode:    Code 0 is reference black.
    DWORD bg = (s_range == BLACK_LEVEL_RANGE_LIMITED) ? D3DCOLOR_XRGB(16, 16, 16) : D3DCOLOR_XRGB(0, 0, 0);
    Terminal_DrawFilledRect(0.0f, 0.0f, sw, sh, bg);

    // 2. Header
    Terminal_PrintRowCenter(1, COLOR_PHOSPHOR_BRIGHT, "=== BLACK LEVEL & PLUGE VERIFIER ===");
    Terminal_PrintRowCenter(2, COLOR_PHOSPHOR_NORMAL,
                            "RANGE: [ %s ]  ((X) to toggle)",
                            (s_range == BLACK_LEVEL_RANGE_LIMITED) ?
                            "LIMITED 16-235 (Standard Broadcast / HDMI Video)" :
                            "FULL 0-255 (PC Monitor / Direct RGB)");

    // 3. Shadow Detail / Near-Black Steps
    Terminal_PrintRow(3, COLOR_PHOSPHOR_DIM,
                      (s_range == BLACK_LEVEL_RANGE_LIMITED) ?
                      "SHADOW DETAIL & SUB-BLACK STEPS (Reference Black = Code 16):" :
                      "SHADOW DETAIL & NEAR-BLACK STEPS (Reference Black = Code 0):");

    static const uint8_t kLimitedShadows[13] = { 0, 4, 8, 12, 16, 17, 18, 19, 20, 22, 24, 28, 32 };
    static const uint8_t kFullShadows[13]    = { 0, 1, 2,  3,  4,  6,  8, 10, 12, 16, 20, 24, 32 };

    const uint8_t *shadowSteps = (s_range == BLACK_LEVEL_RANGE_LIMITED) ? kLimitedShadows : kFullShadows;

    float startX = sw * 0.06f;
    float totalW = sw * 0.88f;
    float gap = (sh >= 720.0f) ? 6.0f : 3.0f;
    float boxW = (totalW - (12.0f * gap)) / 13.0f;
    float boxH = (sh >= 720.0f) ? 44.0f : 20.0f;
    float boxY = (float)topM + (float)(4 * lh) + 2.0f;

    for (int i = 0; i < 13; i++) {
        float bx = startX + (float)i * (boxW + gap);
        uint8_t code = shadowSteps[i];
        DWORD col = D3DCOLOR_XRGB(code, code, code);
        Terminal_DrawFilledRect(bx, boxY, boxW, boxH, col);

        DWORD border;
        if ((s_range == BLACK_LEVEL_RANGE_LIMITED && code == 16) ||
            (s_range == BLACK_LEVEL_RANGE_FULL && code == 0)) {
            border = COLOR_PHOSPHOR_BRIGHT;
        } else {
            border = COLOR_PHOSPHOR_DARK;
        }
        Terminal_DrawRect(bx, boxY, boxW, boxH, border);

        // Print code label under box
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%u", code);
        Terminal_PrintAt((int)(bx + 2.0f), (int)(boxY + boxH + 2.0f),
                         (border == COLOR_PHOSPHOR_BRIGHT) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM,
                         lbl);
    }

    // 4. Centered Broadcast Standard PLUGE Bars
    float plugeH = (sh >= 720.0f) ? 140.0f : 66.0f;
    float plugeW = (sh >= 720.0f) ? 440.0f : 240.0f;
    float plugeX = (sw - plugeW) / 2.0f;
    float plugeY = boxY + boxH + (float)lh + 16.0f;

    // Outer border around PLUGE field
    Terminal_DrawRectThick(plugeX, plugeY, plugeW, plugeH, COLOR_PHOSPHOR_DIM);

    // 4 vertical test bars
    float barW = plugeW / 4.0f;
    uint8_t plugeCodes[4];
    if (s_range == BLACK_LEVEL_RANGE_LIMITED) {
        plugeCodes[0] = 8;   // Sub-black (-4%)
        plugeCodes[1] = 16;  // Reference Black (0%)
        plugeCodes[2] = 20;  // Above-black (+2%)
        plugeCodes[3] = 24;  // Above-black (+4%)
    } else {
        plugeCodes[0] = 0;   // Reference Black (0)
        plugeCodes[1] = 4;   // Near-black
        plugeCodes[2] = 12;  // Above-black
        plugeCodes[3] = 24;  // Highlight
    }

    const char *plugeLabels[4] = {
        (s_range == BLACK_LEVEL_RANGE_LIMITED) ? "Sub-8"  : "0 Ref",
        (s_range == BLACK_LEVEL_RANGE_LIMITED) ? "16 Ref" : "+4",
        (s_range == BLACK_LEVEL_RANGE_LIMITED) ? "+2(20)" : "+12",
        (s_range == BLACK_LEVEL_RANGE_LIMITED) ? "+4(24)" : "+24"
    };

    for (int b = 0; b < 4; b++) {
        float bx = plugeX + (float)b * barW;
        DWORD bCol = D3DCOLOR_XRGB(plugeCodes[b], plugeCodes[b], plugeCodes[b]);
        Terminal_DrawFilledRect(bx, plugeY, barW, plugeH, bCol);
        Terminal_DrawRect(bx, plugeY, barW, plugeH, COLOR_PHOSPHOR_DARK);

        Terminal_PrintAt((int)(bx + 4.0f), (int)(plugeY + plugeH - (float)lh - 2.0f),
                         COLOR_PHOSPHOR_NORMAL, plugeLabels[b]);
    }

    // 5. Highlight Steps (Near-White & Peak White Contrast)
    float whiteY = plugeY + plugeH + 18.0f;
    int whiteRow = (int)((whiteY - (float)topM) / (float)lh);

    Terminal_PrintRow(whiteRow, COLOR_PHOSPHOR_DIM,
                      (s_range == BLACK_LEVEL_RANGE_LIMITED) ?
                      "HIGHLIGHT DETAIL (Reference White = Code 235, Peak = 255):" :
                      "HIGHLIGHT DETAIL (Linear Dynamic Ramp to Peak White = 255):");

    static const uint8_t kWhiteSteps[9] = { 210, 220, 225, 230, 235, 240, 245, 250, 255 };
    float whiteBoxW = (totalW - (8.0f * gap)) / 9.0f;
    float whiteBoxH = boxH;
    float whiteBoxesY = (float)topM + (float)((whiteRow + 1) * lh) + 2.0f;

    for (int i = 0; i < 9; i++) {
        float wx = startX + (float)i * (whiteBoxW + gap);
        uint8_t wCode = kWhiteSteps[i];
        DWORD wCol = D3DCOLOR_XRGB(wCode, wCode, wCode);
        Terminal_DrawFilledRect(wx, whiteBoxesY, whiteBoxW, whiteBoxH, wCol);

        DWORD border = (s_range == BLACK_LEVEL_RANGE_LIMITED && wCode == 235) ?
                       COLOR_PHOSPHOR_BRIGHT : ((wCode == 255) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DARK);
        Terminal_DrawRect(wx, whiteBoxesY, whiteBoxW, whiteBoxH, border);

        char wLbl[8];
        snprintf(wLbl, sizeof(wLbl), "%u", wCode);
        Terminal_PrintAt((int)(wx + 2.0f), (int)(whiteBoxesY + whiteBoxH + 2.0f),
                         (border == COLOR_PHOSPHOR_BRIGHT) ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_DIM,
                         wLbl);
    }

    // 6. Educational Calibration Notes
    int bottomRow = (sh <= 480.0f) ? 24 : 23;
    if (sh > 480.0f) {
        if (s_range == BLACK_LEVEL_RANGE_LIMITED) {
            Terminal_PrintRow(bottomRow - 4, COLOR_PHOSPHOR_AMBER,
                              "  * Sub-black 0-12 MUST blend into background. If visible, TV Brightness too high!");
            Terminal_PrintRow(bottomRow - 3, COLOR_PHOSPHOR_NORMAL,
                              "  * Code 18-20 should be barely visible against 16. If 18 is black -> BLACK CRUSH!");
            Terminal_PrintRow(bottomRow - 2, COLOR_PHOSPHOR_DIM,
                              "  * Highlights: Code 235 should be distinguishable from 240+ without clipping.");
        } else {
            Terminal_PrintRow(bottomRow - 4, COLOR_PHOSPHOR_AMBER,
                              "  * Code 0 is true black; Code 1-4 should be barely perceptible above black.");
            Terminal_PrintRow(bottomRow - 3, COLOR_PHOSPHOR_NORMAL,
                              "  * Check smooth shadow gradient from 0 to 32 without banding or crush.");
            Terminal_PrintRow(bottomRow - 2, COLOR_PHOSPHOR_DIM,
                              "  * Peak white is Code 255. Verify no clipping across 210-255 steps.");
        }
    }

    // 7. Footer
    Terminal_DrawHLine(bottomRow - 1, '-', COLOR_PHOSPHOR_DARK);
    Terminal_PrintRow(bottomRow, COLOR_PHOSPHOR_BRIGHT,
                      "KEYS: (X): Toggle Limited (16-235) / Full (0-255) | (B): Return to Alignment");
}
