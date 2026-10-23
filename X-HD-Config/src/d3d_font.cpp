#include "d3d_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
    extern const unsigned char g_fontXprData[];
    extern const size_t g_fontXprSize;
}

#define XPR0_MAGIC_VALUE 0x30525058
#define XPR1_MAGIC_VALUE 0x31525058

#pragma pack(push, 1)
struct XPR_HEADER {
    DWORD dwMagic;
    DWORD dwTotalSize;
    DWORD dwHeaderSize;
};
#pragma pack(pop)

struct GLYPH_ATTR {
    FLOAT tu1, tv1, tu2, tv2; // Texture coordinates
    SHORT wOffset;            // Pixel offset for glyph start
    SHORT wWidth;             // Pixel width of glyph
    SHORT wAdvance;           // Pixels to advance after glyph
};

// Fixed-Function 2D textured quad vertex structure
struct D3D_FONT_VERTEX {
    FLOAT x, y, z, rhw; // Screen pos (z=0.0f, rhw=1.0f)
    DWORD color;        // Diffuse color (D3DCOLOR)
    FLOAT u, v;         // Texture coordinates
};

#define D3DFVF_FONT_VERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static D3DTexture        *g_pFontTexture       = NULL;
static BYTE              *g_pSysMemData        = NULL;
static BYTE              *g_pVidMemData        = NULL;

static FLOAT              g_fFontHeight        = 27.0f;
static FLOAT              g_fFontTopPadding    = 1.0f;
static FLOAT              g_fFontBottomPadding = 2.0f;
static FLOAT              g_fFontYAdvance      = 24.0f;

static FLOAT              g_fScaleX            = 1.0f;
static FLOAT              g_fScaleY            = 1.0f;

static WORD               g_cMaxGlyph          = 0;
static const SHORT       *g_pTranslatorTable   = NULL;
static DWORD              g_dwNumGlyphs        = 0;
static const GLYPH_ATTR  *g_pGlyphs            = NULL;

static bool               g_bInitialized       = false;

static const unsigned char* load_xpr_data(DWORD *outTotalSize, bool *outAllocated)
{
    *outAllocated = false;
    const char *paths[] = {
        "D:\\Media\\Font.xpr",
        "D:\\Font.xpr",
        "Font.xpr",
        NULL
    };

    for (int i = 0; paths[i] != NULL; i++) {
        HANDLE hFile = CreateFileA(paths[i], GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD dwSize = GetFileSize(hFile, NULL);
            if (dwSize > sizeof(XPR_HEADER)) {
                BYTE *buf = (BYTE*)malloc(dwSize);
                if (buf) {
                    DWORD bytesRead = 0;
                    if (ReadFile(hFile, buf, dwSize, &bytesRead, NULL) && bytesRead == dwSize) {
                        CloseHandle(hFile);
                        *outTotalSize = dwSize;
                        *outAllocated = true;
                        return buf;
                    }
                    free(buf);
                }
            }
            CloseHandle(hFile);
        }
    }

    // Fallback to embedded byte array
    if (g_fontXprSize > sizeof(XPR_HEADER)) {
        *outTotalSize = (DWORD)g_fontXprSize;
        *outAllocated = false;
        return g_fontXprData;
    }

    return NULL;
}

bool D3DFont_Init(void)
{
    if (g_bInitialized) {
        return true;
    }

    DWORD totalSize = 0;
    bool bAllocated = false;
    const unsigned char *pRaw = load_xpr_data(&totalSize, &bAllocated);
    if (!pRaw || totalSize < sizeof(XPR_HEADER)) {
        if (bAllocated && pRaw) free((void*)pRaw);
        return false;
    }

    const XPR_HEADER *hdr = (const XPR_HEADER*)pRaw;
    if (hdr->dwMagic != XPR0_MAGIC_VALUE && hdr->dwMagic != XPR1_MAGIC_VALUE) {
        if (bAllocated) free((void*)pRaw);
        return false;
    }

    DWORD dwSysMemSize = hdr->dwHeaderSize - sizeof(XPR_HEADER);
    DWORD dwVidMemSize = hdr->dwTotalSize - hdr->dwHeaderSize;

    g_pSysMemData = (BYTE*)malloc(dwSysMemSize);
    g_pVidMemData = (BYTE*)D3D_AllocContiguousMemory(dwVidMemSize, D3DTEXTURE_ALIGNMENT);

    if (!g_pSysMemData || !g_pVidMemData) {
        if (g_pSysMemData) { free(g_pSysMemData); g_pSysMemData = NULL; }
        if (g_pVidMemData) { D3D_FreeContiguousMemory(g_pVidMemData); g_pVidMemData = NULL; }
        if (bAllocated) free((void*)pRaw);
        return false;
    }

    memcpy(g_pSysMemData, pRaw + sizeof(XPR_HEADER), dwSysMemSize);
    memcpy(g_pVidMemData, pRaw + hdr->dwHeaderSize, dwVidMemSize);

    if (bAllocated) {
        free((void*)pRaw);
    }

    // Offset 0 of SysMemData is the D3DTexture struct
    g_pFontTexture = (D3DTexture*)g_pSysMemData;
    D3DTexture_Register(g_pFontTexture, g_pVidMemData);
    D3DResource_AddRef((D3DResource*)g_pFontTexture);

    // Font metadata follows D3DTexture
    BYTE *pData = g_pSysMemData + sizeof(D3DTexture);
    pData += 2 * sizeof(DWORD); // skip dwResourceType and dwResourceSize
    DWORD dwFileVersion = *((DWORD*)pData);
    pData += sizeof(DWORD);

    if (dwFileVersion == 0x00000005) {
        g_fFontHeight        = *((FLOAT*)pData); pData += sizeof(FLOAT);
        g_fFontTopPadding    = *((FLOAT*)pData); pData += sizeof(FLOAT);
        g_fFontBottomPadding = *((FLOAT*)pData); pData += sizeof(FLOAT);
        g_fFontYAdvance      = *((FLOAT*)pData); pData += sizeof(FLOAT);

        g_cMaxGlyph          = ((WORD*)pData)[0];
        pData += sizeof(WORD);
        g_pTranslatorTable   = (const SHORT*)pData;
        pData += sizeof(WCHAR) * (g_cMaxGlyph + 1);

        g_dwNumGlyphs        = ((DWORD*)pData)[0];
        pData += sizeof(DWORD);
        g_pGlyphs            = (const GLYPH_ATTR*)pData;
    } else {
        D3DFont_Shutdown();
        return false;
    }

    g_bInitialized = true;
    return true;
}

void D3DFont_Shutdown(void)
{
    if (g_pVidMemData) {
        D3D_FreeContiguousMemory(g_pVidMemData);
        g_pVidMemData = NULL;
    }
    if (g_pSysMemData) {
        free(g_pSysMemData);
        g_pSysMemData = NULL;
    }
    g_pFontTexture     = NULL;
    g_pTranslatorTable = NULL;
    g_pGlyphs          = NULL;
    g_bInitialized     = false;
}

void D3DFont_SetScale(float scaleX, float scaleY)
{
    g_fScaleX = (scaleX > 0.05f) ? scaleX : 0.05f;
    g_fScaleY = (scaleY > 0.05f) ? scaleY : 0.05f;
}

void D3DFont_GetScale(float *scaleX, float *scaleY)
{
    if (scaleX) *scaleX = g_fScaleX;
    if (scaleY) *scaleY = g_fScaleY;
}

float D3DFont_GetFontHeight(void)
{
    return g_fFontHeight * g_fScaleY;
}

float D3DFont_GetLineAdvance(void)
{
    return g_fFontYAdvance * g_fScaleY;
}

bool D3DFont_GetTextExtent(const WCHAR *str, int len, float *pWidth, float *pHeight)
{
    if (!str || !g_bInitialized) return false;

    float sx = 0.0f;
    float maxW = 0.0f;
    float sy = g_fFontHeight;

    int i = 0;
    while ((len < 0 || i < len) && str[i] != 0) {
        WCHAR letter = str[i++];
        if (letter == L'\n') {
            if (sx > maxW) maxW = sx;
            sx = 0.0f;
            sy += g_fFontYAdvance;
            continue;
        }

        const GLYPH_ATTR *pGlyph = &g_pGlyphs[0];
        if (letter <= g_cMaxGlyph && g_pTranslatorTable[letter] > 0 && (DWORD)g_pTranslatorTable[letter] < g_dwNumGlyphs) {
            pGlyph = &g_pGlyphs[g_pTranslatorTable[letter]];
        }

        sx += (float)pGlyph->wOffset;
        sx += (float)pGlyph->wAdvance;
    }
    if (sx > maxW) maxW = sx;

    if (pWidth)  *pWidth  = maxW * g_fScaleX;
    if (pHeight) *pHeight = sy * g_fScaleY;
    return true;
}

float D3DFont_GetTextWidth(const WCHAR *str, int len)
{
    float w = 0.0f;
    D3DFont_GetTextExtent(str, len, &w, NULL);
    return w;
}

// Batch size must be <= 18 quads (72 vertices). DrawVerticesUP in Xbox D3D chunks
// at 2047 / sizeof(vertex) = 73 vertices. If a batch exceeds 73 vertices, DrawVerticesUP
// splits the batch across push-buffer method packets, slicing a quad in half (18 quads + 1 vertex),
// which corrupts the 19th quad coordinates and misaligns push-buffer command words.
// Using 16 quads (64 vertices) guarantees every batch is emitted in a single unfragmented packet.
#define FONT_BATCH_SIZE 16

static bool s_fontStatesConfigured = false;

void D3DFont_InvalidateState(void)
{
    s_fontStatesConfigured = false;
}

void D3DFont_DrawText(float x, float y, DWORD color, const WCHAR *str, int len)
{
    if (!str || !g_bInitialized || len == 0) return;

    if (!s_fontStatesConfigured) {
        // Set fixed-function 2D textured pipeline states once
        D3DDevice_SetVertexShader(D3DFVF_FONT_VERTEX);
        D3DDevice_SetPixelShader(0);
        D3DDevice_SetTexture(0, (D3DBaseTexture*)g_pFontTexture);

        // Standard texture modulate stages (diffuse * texture color & alpha)
        D3DDevice_SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
        D3DDevice_SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        D3DDevice_SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        D3DDevice_SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
        D3DDevice_SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        D3DDevice_SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        // Clamp texture addressing to eliminate edge sampling artifacts / "blips"
        D3DDevice_SetTextureStageState(0, D3DTSS_ADDRESSU,  D3DTADDRESS_CLAMP);
        D3DDevice_SetTextureStageState(0, D3DTSS_ADDRESSV,  D3DTADDRESS_CLAMP);
        D3DDevice_SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        D3DDevice_SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

        // Alpha blending and testing
        D3DDevice_SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        D3DDevice_SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
        D3DDevice_SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
        D3DDevice_SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
        D3DDevice_SetRenderState(D3DRS_ALPHAREF,         0x08);
        D3DDevice_SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);

        D3DDevice_SetRenderState(D3DRS_LIGHTING,         FALSE);
        D3DDevice_SetRenderState(D3DRS_FILLMODE,         D3DFILL_SOLID);
        D3DDevice_SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
        D3DDevice_SetRenderState(D3DRS_ZENABLE,          FALSE);
        D3DDevice_SetRenderState(D3DRS_STENCILENABLE,    FALSE);
        D3DDevice_SetRenderState(D3DRS_EDGEANTIALIAS,    FALSE);
        s_fontStatesConfigured = true;
    }

    // DirectX screen-space half-pixel offset (-0.5f) for pixel-perfect texel alignment
    float curX = floorf(x) - 0.5f;
    float curY = floorf(y - g_fFontTopPadding * g_fScaleY) - 0.5f;
    float originX = curX;

    static D3D_FONT_VERTEX batch[FONT_BATCH_SIZE * 4];
    int quadCount = 0;

    int i = 0;
    while ((len < 0 || i < len) && str[i] != 0) {
        WCHAR letter = str[i++];
        if (letter == L'\n') {
            curX = originX;
            curY += g_fFontYAdvance * g_fScaleY;
            continue;
        }

        const GLYPH_ATTR *pGlyph = &g_pGlyphs[0];
        if (letter <= g_cMaxGlyph && g_pTranslatorTable[letter] > 0 && (DWORD)g_pTranslatorTable[letter] < g_dwNumGlyphs) {
            pGlyph = &g_pGlyphs[g_pTranslatorTable[letter]];
        }

        float fOffset  = g_fScaleX * (float)pGlyph->wOffset;
        float fAdvance = g_fScaleX * (float)pGlyph->wAdvance;
        float fWidth   = g_fScaleX * (float)pGlyph->wWidth;
        float fHeight  = g_fScaleY * g_fFontHeight;

        curX += fOffset;
        float left   = curX;
        float right  = left + fWidth;
        float top    = curY;
        float bottom = top + fHeight;
        curX += fAdvance;

        int baseIdx = quadCount * 4;
        batch[baseIdx + 0].x   = left;
        batch[baseIdx + 0].y   = bottom;
        batch[baseIdx + 0].z   = 0.0f;
        batch[baseIdx + 0].rhw = 1.0f;
        batch[baseIdx + 0].color = color;
        batch[baseIdx + 0].u   = pGlyph->tu1;
        batch[baseIdx + 0].v   = pGlyph->tv2;

        batch[baseIdx + 1].x   = left;
        batch[baseIdx + 1].y   = top;
        batch[baseIdx + 1].z   = 0.0f;
        batch[baseIdx + 1].rhw = 1.0f;
        batch[baseIdx + 1].color = color;
        batch[baseIdx + 1].u   = pGlyph->tu1;
        batch[baseIdx + 1].v   = pGlyph->tv1;

        batch[baseIdx + 2].x   = right;
        batch[baseIdx + 2].y   = top;
        batch[baseIdx + 2].z   = 0.0f;
        batch[baseIdx + 2].rhw = 1.0f;
        batch[baseIdx + 2].color = color;
        batch[baseIdx + 2].u   = pGlyph->tu2;
        batch[baseIdx + 2].v   = pGlyph->tv1;

        batch[baseIdx + 3].x   = right;
        batch[baseIdx + 3].y   = bottom;
        batch[baseIdx + 3].z   = 0.0f;
        batch[baseIdx + 3].rhw = 1.0f;
        batch[baseIdx + 3].color = color;
        batch[baseIdx + 3].u   = pGlyph->tu2;
        batch[baseIdx + 3].v   = pGlyph->tv2;

        quadCount++;
        if (quadCount >= FONT_BATCH_SIZE) {
            D3DDevice_DrawVerticesUP(D3DPT_QUADLIST, quadCount * 4, batch, sizeof(D3D_FONT_VERTEX));
            quadCount = 0;
        }
    }

    if (quadCount > 0) {
        D3DDevice_DrawVerticesUP(D3DPT_QUADLIST, quadCount * 4, batch, sizeof(D3D_FONT_VERTEX));
    }
}
