// SPDX-License-Identifier: GPL-3.0-only
// The picture test pattern. Everything is in framebuffer codes (0 black, 255 white); the
// console's encoder turns them into video-level YCbCr, and the X-HD's colour space and picture
// settings decide what reaches the TV. The pattern never changes -- only the board does -- so
// what's judged is the board and the TV, not the drawing. Exactly where each encoder puts
// framebuffer 0 hasn't been measured, which is why this is judged on the TV, not computed.
#include "ui_pattern.h"
#include "ui_terminal.h"
#include <stdio.h>

#define GAP 4.0f

static void boxes(const uint8_t *codes, int n, float x, float w, float y, float h)
{
    const float bw = (w - (float)(n - 1) * GAP) / (float)n;
    for (int i = 0; i < n; i++) {
        const float bx = x + (float)i * (bw + GAP);
        Terminal_DrawFilledRect(bx, y, bw, h, D3DCOLOR_XRGB(codes[i], codes[i], codes[i]));
        Terminal_DrawRect(bx, y, bw, h, COLOR_PHOSPHOR_DARK);
        Terminal_PrintAt((int)(bx + 2.0f), (int)(y + h + 1.0f), COLOR_PHOSPHOR_DIM, "%u", codes[i]);
    }
}

void Pattern_Draw(float x, float y, float w, float h)
{
    const int lh = Terminal_GetLineHeight();

    // Background: framebuffer code 0, the black every step is judged against.
    Terminal_DrawFilledRect(x, y, w, h, D3DCOLOR_XRGB(0, 0, 0));
    Terminal_DrawRect(x, y, w, h, COLOR_PHOSPHOR_DARK);

    // Three captions and two rows of step numbers are text; the four blocks share the rest.
    const float pad = 8.0f;
    x += pad;
    w -= 2.0f * pad;
    float free = h - 2.0f * pad - 5.0f * (float)lh - 3.0f * pad;
    if (free < 80.0f) free = 80.0f;
    const float stepH  = free * 0.17f;
    const float plugeH = free * 0.38f;
    const float barsH  = free * 0.28f;
    float cy = y + pad;

    // 1. Near-black steps
    static const uint8_t kShadows[13] = { 0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32 };
    Terminal_PrintAt((int)x, (int)cy, COLOR_PHOSPHOR_DIM, "NEAR-BLACK STEPS (background = 0)");
    cy += (float)lh;
    boxes(kShadows, 13, x, w, cy, stepH);
    cy += stepH + (float)lh + pad;

    // 2. Large near-black bars: easier to judge from across the room than the small steps
    static const uint8_t kBars[4] = { 0, 4, 8, 16 };
    const float barW = w / 4.0f;
    for (int b = 0; b < 4; b++) {
        const float bx = x + (float)b * barW;
        Terminal_DrawFilledRect(bx, cy, barW, plugeH, D3DCOLOR_XRGB(kBars[b], kBars[b], kBars[b]));
        Terminal_DrawRect(bx, cy, barW, plugeH, COLOR_PHOSPHOR_DARK);
        Terminal_PrintAt((int)(bx + 4.0f), (int)(cy + plugeH - (float)lh), COLOR_PHOSPHOR_NORMAL, "%u", kBars[b]);
    }
    cy += plugeH + pad;

    // 3. Near-white steps: where the white clip and the TV's own limit show
    static const uint8_t kWhites[9] = { 210, 220, 225, 230, 235, 240, 245, 250, 255 };
    Terminal_PrintAt((int)x, (int)cy, COLOR_PHOSPHOR_DIM, "NEAR-WHITE STEPS (peak = 255)");
    cy += (float)lh;
    boxes(kWhites, 9, x, w, cy, stepH);
    cy += stepH + (float)lh + pad;

    // 4. 75% colour bars, for saturation (and any colour cast)
    static const DWORD kColours[7] = {
        D3DCOLOR_XRGB(191, 191, 191), D3DCOLOR_XRGB(191, 191, 0), D3DCOLOR_XRGB(0, 191, 191),
        D3DCOLOR_XRGB(0, 191, 0),     D3DCOLOR_XRGB(191, 0, 191), D3DCOLOR_XRGB(191, 0, 0),
        D3DCOLOR_XRGB(0, 0, 191),
    };
    Terminal_PrintAt((int)x, (int)cy, COLOR_PHOSPHOR_DIM, "75%% COLOUR BARS");
    cy += (float)lh;
    const float cw = w / 7.0f;
    for (int i = 0; i < 7; i++) {
        Terminal_DrawFilledRect(x + (float)i * cw, cy, cw, barsH, kColours[i]);
    }
}
