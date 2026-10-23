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
bool Terminal_Init(void);
bool Terminal_SetVideoMode(int width, int height, DWORD d3d_flags, UINT refreshRate);
void Terminal_BeginFrame(void);
void Terminal_EndFrame(void);
void Terminal_FlushBatch(void);

int  Terminal_GetWidth(void);
int  Terminal_GetHeight(void);
int  Terminal_GetLineHeight(void);
int  Terminal_GetTopMargin(void);
int  Terminal_GetLeftMargin(void);

// Text rendering primitives
void Terminal_PrintAt(int x, int y, DWORD color, const char *fmt, ...);
void Terminal_PrintRow(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowIndent(int row, int indentPx, DWORD color, const char *fmt, ...);
void Terminal_PrintRowRight(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowCenter(int row, DWORD color, const char *fmt, ...);
void Terminal_PrintRowCenterIn(int row, int leftIndentPx, int widthPx, DWORD color, const char *fmt, ...);
void Terminal_PrintCentered(DWORD color, const char *fmt, ...);

// CRT Raster Collapse Shutdown Sequence & Dashboard Launch
void UI_RunCRTCollapseAndExit(void);

// Geometric retro UI primitives
void Terminal_DrawHLine(int row, char ch, DWORD color);
void Terminal_DrawLine(float x1, float y1, float x2, float y2, DWORD color);
void Terminal_DrawLineThick(float x1, float y1, float x2, float y2, DWORD color);
void Terminal_DrawRect(float x, float y, float w, float h, DWORD color);
void Terminal_DrawRectThick(float x, float y, float w, float h, DWORD color);
void Terminal_DrawFilledRect(float x, float y, float w, float h, DWORD color);
void Terminal_DrawAlignmentReticle(int pos_h, int pos_v);
void Terminal_DrawHeader(const char *activeTabName, const char *fwVer);
void Terminal_DrawFooter(const char *shortcuts, const char *shortcuts2, const char *statusMsg);

#ifdef __cplusplus
}
#endif
