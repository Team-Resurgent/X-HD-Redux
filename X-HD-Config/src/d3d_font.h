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

// Set scale factors for current video mode
void  D3DFont_SetScale(float scaleX, float scaleY);
void  D3DFont_GetScale(float *scaleX, float *scaleY);

// Get font metrics
float D3DFont_GetFontHeight(void);
float D3DFont_GetLineAdvance(void);

// Measure text dimensions
bool  D3DFont_GetTextExtent(const WCHAR *str, int len, float *pWidth, float *pHeight);
float D3DFont_GetTextWidth(const WCHAR *str, int len);

// Render text string as textured 2D quads
void  D3DFont_DrawText(float x, float y, DWORD color, const WCHAR *str, int len);

// Invalidate cached font pipeline states (called when non-font drawing alters render states)
void  D3DFont_InvalidateState(void);

#ifdef __cplusplus
}
#endif
