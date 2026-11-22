// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <d3d8.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Direct3D font system using Font.xpr (from disk or embedded)
bool  D3DFont_Init(void);

// Release font resources
void  D3DFont_Shutdown(void);

// Text is laid out and measured in 1280x720 terms; drawing scales it by these factors to the
// back buffer (1.0 at 720p).
void  D3DFont_SetOutputScale(float scaleX, float scaleY);

// Measure text dimensions (1280x720 terms)
bool  D3DFont_GetTextExtent(const WCHAR *str, int len, float *pWidth, float *pHeight);
float D3DFont_GetTextWidth(const WCHAR *str, int len);

// Layout advance of a single glyph (1280x720 terms); the atlas is proportional
float D3DFont_GetGlyphAdvance(WCHAR ch);

// Render text string as textured 2D quads
void  D3DFont_DrawText(float x, float y, DWORD color, const WCHAR *str, int len);

// Render text with every character centered in a fixed-width cell, so columns
// line up like a monospace terminal even though the atlas is proportional
void  D3DFont_DrawTextMono(float x, float y, DWORD color, const WCHAR *str, int len, float cellW);

// Invalidate cached font pipeline states (called when non-font drawing alters render states)
void  D3DFont_InvalidateState(void);

#ifdef __cplusplus
}
#endif
