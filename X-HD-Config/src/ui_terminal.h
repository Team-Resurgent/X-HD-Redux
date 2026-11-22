// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <d3d8.h>
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

// Phosphor Color Palette (ARGB)
#define COLOR_PHOSPHOR_BRIGHT       0xFF66FF66  // Selected item, cursor, bright glow
#define COLOR_PHOSPHOR_NORMAL       0xFF28DD28  // Standard body text
#define COLOR_PHOSPHOR_DIM          0xFF147A14  // Labels, frames, hints
#define COLOR_PHOSPHOR_DARK         0xFF0A440A  // Subtle separators
#define COLOR_PHOSPHOR_ACCENT       0xFF99FF99  // Titles, active tabs
#define COLOR_PHOSPHOR_AMBER        0xFFEEEE44  // Notifications, alerts

// Terminal Engine APIs
// Opens the device in this mode; false if the console refuses it. Layout is always 1280x720.
bool Terminal_Init(int width, int height, DWORD d3d_flags, UINT refreshRate);
bool Terminal_SetVideoMode(int width, int height, DWORD d3d_flags, UINT refreshRate);
void Terminal_BeginFrame(void);
void Terminal_EndFrame(void);
void Terminal_FlushBatch(void);

// Layout size: always 1280x720, whatever the back buffer is.
int  Terminal_GetWidth(void);
int  Terminal_GetHeight(void);
int  Terminal_GetLineHeight(void);
int  Terminal_GetTopMargin(void);
int  Terminal_GetLeftMargin(void);

// Text rendering primitives
// Width of plain text in 1280x720 layout pixels.
int  Terminal_TextWidth(const char *text);
void Terminal_PrintAt(int x, int y, DWORD color, const char *fmt, ...);
void Terminal_PrintRow(int row, DWORD color, const char *fmt, ...);
// Fixed-width character cells (hex dumps and other column layouts); the cell is
// sized to the widest hex-dump glyph so printf space-padding lines up on screen.
void Terminal_PrintRowMono(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowIndent(int row, int indentPx, DWORD color, const char *fmt, ...);
void Terminal_PrintRowRight(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowCenter(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowCenterIn(int row, int leftIndentPx, int widthPx, DWORD color, const char *fmt, ...);
void Terminal_PrintCentered(DWORD color, const char *fmt, ...);
// A menu item: "> text" in bright when selected, "  text" in normal otherwise.
void Terminal_PrintItem(int row, bool selected, const char *fmt, ...);

// CRT Raster Collapse Shutdown Sequence & Dashboard Launch
void UI_RunCRTCollapse(void);
void UI_RunCRTCollapseAndExit(void);

// Geometric retro UI primitives
void Terminal_DrawHLine(int row, char ch, DWORD color);
void Terminal_DrawLine(float x1, float y1, float x2, float y2, DWORD color);
void Terminal_DrawLineThick(float x1, float y1, float x2, float y2, DWORD color);
void Terminal_DrawRect(float x, float y, float w, float h, DWORD color);
void Terminal_DrawRectThick(float x, float y, float w, float h, DWORD color);
void Terminal_DrawFilledRect(float x, float y, float w, float h, DWORD color);
void Terminal_DrawAlignmentReticle(int pos_h, int pos_v);
#define TERMINAL_MAX_TABS 12

typedef struct {
    const char *const *tab_names;   // shown for the active tab only; the others are numbers
    int   tab_count;
    int   active_tab;
    const char *board_text;         // right of the title: "FW: v2.0.0", "NO X-HD BOARD", ...
    DWORD board_color;
    bool  unsaved;                  // settings changed since the last save: flagged in amber
} terminal_header_t;

// Rows 0-4: title, board state, the tab strip.
void Terminal_DrawHeader(const terminal_header_t *h);
// The footer's top row (its separator line); content must end above it.
int  Terminal_FooterTopRow(void);
// The message line (empty when there's nothing to say: the header already shows the board
// and unsaved state) and the key row. An alert (an error, an action's outcome) is amber.
void Terminal_DrawFooter(const char *shortcuts, const char *statusMsg, bool alert);

#ifdef __cplusplus
}
#endif
