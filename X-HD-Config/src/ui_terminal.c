#include "ui_terminal.h"
#include "d3d_font.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static D3DDevice  *g_pDevice = NULL;

static int g_screenWidth  = 1280;
static int g_screenHeight = 720;
static int g_marginLeft   = 80;
static int g_marginRight  = 80;
static int g_marginTop    = 36;
static int g_lineHeight   = 26;

int Terminal_GetWidth(void)      { return g_screenWidth; }
int Terminal_GetHeight(void)     { return g_screenHeight; }
int Terminal_GetLineHeight(void) { return g_lineHeight; }
int Terminal_GetTopMargin(void)  { return g_marginTop; }
int Terminal_GetLeftMargin(void) { return g_marginLeft; }

static void update_font_and_layout(int width, int height)
{
    g_screenWidth  = width;
    g_screenHeight = height;

    if (height <= 480) {
        g_marginLeft  = (width >= 720) ? 40 : 36;
        g_marginRight = (width >= 720) ? 40 : 36;
        g_marginTop   = 28;
        g_lineHeight  = 17;
        D3DFont_SetScale(0.60f, 0.60f);
    } else if (height <= 720) {
        g_marginLeft  = 80;
        g_marginRight = 80;
        g_marginTop   = 36;
        g_lineHeight  = 26;
        D3DFont_SetScale(1.0f, 1.0f);
    } else { // 1080i
        g_marginLeft  = 120;
        g_marginRight = 120;
        g_marginTop   = 54;
        g_lineHeight  = 35;
        D3DFont_SetScale(1.42f, 1.42f);
    }
}

bool Terminal_Init(void)
{
    D3DPRESENT_PARAMETERS pp;
    Direct3D *pD3D;
    D3DVIEWPORT8 vp;
    HRESULT hr;

    pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!pD3D) {
        return false;
    }

    // Try native 720p (1280x720 progressive widescreen) first
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.Windowed = FALSE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_RefreshRateInHz = 60;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp.Flags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;


    hr = Direct3D_CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_pDevice);

    int initW = 1280, initH = 720;
    if (FAILED(hr)) {
        // Fallback to standard 640x480 if 720p is not enabled on the Xbox AV pack
        ZeroMemory(&pp, sizeof(pp));
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.Windowed = FALSE;
        pp.EnableAutoDepthStencil = FALSE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.FullScreen_RefreshRateInHz = 60;
        pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

        hr = Direct3D_CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_pDevice);
        if (FAILED(hr)) {
            return false;
        }
        initW = 640;
        initH = 480;
    }

    ZeroMemory(&vp, sizeof(vp));
    vp.Width = pp.BackBufferWidth;
    vp.Height = pp.BackBufferHeight;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    D3DDevice_SetViewport(&vp);

    // Initialize D3D textured quad font renderer (Arial proportional atlas)
    bool fontOk = D3DFont_Init();

    update_font_and_layout(initW, initH);

    return fontOk;
}

bool Terminal_SetVideoMode(int width, int height, DWORD d3d_flags, UINT refreshRate)
{
    if (!g_pDevice) return false;

    // Unbind texture and shader state before device reset
    D3DDevice_SetTexture(0, NULL);
    D3DDevice_SetVertexShader(0);
    D3DDevice_SetPixelShader(0);

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.Windowed = FALSE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_RefreshRateInHz = (refreshRate > 0) ? refreshRate : 60;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp.Flags = d3d_flags;

    HRESULT hr = D3DDevice_Reset(&pp);
    if (FAILED(hr)) {
        return false;
    }

    D3DVIEWPORT8 vp;
    ZeroMemory(&vp, sizeof(vp));
    vp.Width = pp.BackBufferWidth;
    vp.Height = pp.BackBufferHeight;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    D3DDevice_SetViewport(&vp);

    update_font_and_layout(width, height);

    D3DDevice_BlockUntilVerticalBlank();
    return true;
}

void Terminal_BeginFrame(void)
{
    if (!g_pDevice) return;

    Terminal_FlushBatch();
    D3DFont_InvalidateState();

    // Pure black phosphor cathode clear
    D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    D3DDevice_BeginScene();
}

void Terminal_EndFrame(void)
{
    if (!g_pDevice) return;

    Terminal_FlushBatch();
    D3DFont_InvalidateState();
    D3DDevice_SetTexture(0, NULL);
    D3DDevice_SetVertexShader(0);
    D3DDevice_EndScene();
    D3DDevice_Swap(0);
    D3DDevice_BlockUntilVerticalBlank();
}

static int format_string(char *buf, size_t bufSize, WCHAR *wbuf, size_t wbufSize, const char *fmt, va_list args)
{
    if (!buf || bufSize == 0 || !wbuf || wbufSize == 0) return 0;
    buf[bufSize - 1] = '\0';
    vsnprintf(buf, bufSize, fmt, args);
    buf[bufSize - 1] = '\0';

    int len = 0;
    while (buf[len] && len < (int)(wbufSize - 1)) {
        wbuf[len] = (WCHAR)(unsigned char)buf[len];
        len++;
    }
    wbuf[len] = 0;
    return len;
}

static void clamp_and_render_text(int x, int y, DWORD color, WCHAR *wbuf, int len)
{
    if (len <= 0) return;
    if (y < 0 || y + g_lineHeight > g_screenHeight) return;

    // Prevent negative x coordinates
    if (x < g_marginLeft) x = g_marginLeft;

    float textWidth = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &textWidth, NULL);

    int maxW = g_screenWidth - g_marginRight - x;
    if (maxW <= 0) return;

    // Safely truncate characters if string exceeds available display width
    while (len > 0 && (int)textWidth > maxW) {
        len--;
        wbuf[len] = 0;
        D3DFont_GetTextExtent(wbuf, len, &textWidth, NULL);
    }

    if (len > 0) {
        Terminal_FlushBatch();
        D3DFont_DrawText((float)x, (float)y, color, wbuf, len);
    }
}

void Terminal_PrintAt(int x, int y, DWORD color, const char *fmt, ...)
{
    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_PrintRow(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    int y = g_marginTop + row * g_lineHeight;

    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    clamp_and_render_text(g_marginLeft, y, color, wbuf, len);
}

void Terminal_PrintRowIndent(int row, int indentPx, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    int y = g_marginTop + row * g_lineHeight;

    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    clamp_and_render_text(g_marginLeft + indentPx, y, color, wbuf, len);
}

void Terminal_PrintRowRight(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    int y = g_marginTop + row * g_lineHeight;

    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    float textWidth = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &textWidth, NULL);

    int x = g_screenWidth - g_marginRight - (int)textWidth;
    if (x < g_marginLeft) x = g_marginLeft;

    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_PrintRowCenter(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    int y = g_marginTop + row * g_lineHeight;

    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    float textWidth = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &textWidth, NULL);

    int x = (g_screenWidth - (int)textWidth) / 2;
    if (x < g_marginLeft) x = g_marginLeft;

    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_PrintRowCenterIn(int row, int leftIndentPx, int widthPx, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    int y = g_marginTop + row * g_lineHeight;

    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    float textWidth = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &textWidth, NULL);

    int x = g_marginLeft + leftIndentPx + (widthPx - (int)textWidth) / 2;
    if (x < g_marginLeft) x = g_marginLeft;

    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_PrintCentered(DWORD color, const char *fmt, ...)
{
    char buf[512];
    WCHAR wbuf[512];
    va_list args;

    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);

    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &textWidth, &textHeight);

    int x = (g_screenWidth - (int)textWidth) / 2;
    int y = (g_screenHeight - (int)textHeight) / 2;
    if (x < g_marginLeft) x = g_marginLeft;

    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_DrawHLine(int row, char ch, DWORD color)
{
    if (row < 0) return;
    float y = (float)(g_marginTop + row * g_lineHeight + (g_lineHeight / 2));
    float x1 = (float)g_marginLeft;
    float x2 = (float)(g_screenWidth - g_marginRight);

    if (ch == '=') {
        Terminal_DrawLine(x1, y - 2.0f, x2, y - 2.0f, color);
        Terminal_DrawLine(x1, y + 1.0f, x2, y + 1.0f, color);
    } else {
        Terminal_DrawLine(x1, y, x2, y, color);
    }
}

void Terminal_DrawHeader(const char *activeTabName, const char *fwVer)
{
    // Double horizontal border
    Terminal_DrawHLine(0, '=', COLOR_PHOSPHOR_ACCENT);

    // Title and right-aligned firmware version
    const char *title = "X-HD SYSTEM CONFIGURATION TERMINAL";
    Terminal_PrintRow(1, COLOR_PHOSPHOR_BRIGHT, "%s", title);

    // Retro blinking phosphor block cursor at end of heading (~2 Hz)
    if ((GetTickCount() / 450) % 2 == 0) {
        WCHAR wTitle[64];
        int wLen = 0;
        while (title[wLen] && wLen < 63) {
            wTitle[wLen] = (WCHAR)(unsigned char)title[wLen];
            wLen++;
        }
        wTitle[wLen] = 0;
        float textW = D3DFont_GetTextWidth(wTitle, wLen);

        float curX = (float)g_marginLeft + textW + 4.0f;
        float curY = (float)(g_marginTop + 1 * g_lineHeight + 3);
        float curW = (g_screenHeight <= 480) ? 7.0f : 10.0f;
        float curH = (float)g_lineHeight - 6.0f;
        Terminal_DrawFilledRect(curX, curY, curW, curH, COLOR_PHOSPHOR_BRIGHT);
    }

    if (fwVer && fwVer[0]) {
        Terminal_PrintRowRight(1, COLOR_PHOSPHOR_NORMAL, "FW: v%s", fwVer);
    }

    Terminal_DrawHLine(2, '=', COLOR_PHOSPHOR_ACCENT);

    // Active Tab Navigation
    Terminal_PrintRow(3, COLOR_PHOSPHOR_DIM, "Tabs: [WHITE / BLACK]   %s", activeTabName ? activeTabName : "MAIN");

    Terminal_DrawHLine(4, '-', COLOR_PHOSPHOR_DARK);
}

void Terminal_DrawFooter(const char *shortcuts, const char *shortcuts2, const char *statusMsg)
{
    int bottomRow = (g_screenHeight <= 480) ? 24 : 23;

    if (shortcuts2 && shortcuts2[0]) {
        Terminal_DrawHLine(bottomRow - 3, '-', COLOR_PHOSPHOR_DARK);

        if (statusMsg && statusMsg[0]) {
            Terminal_PrintRow(bottomRow - 2, COLOR_PHOSPHOR_AMBER, "STATUS: %s", statusMsg);
        } else {
            Terminal_PrintRow(bottomRow - 2, COLOR_PHOSPHOR_DIM, "STATUS: Ready (SMBus @ 0xD2 Active)");
        }

        if (shortcuts && shortcuts[0]) {
            Terminal_PrintRow(bottomRow - 1, COLOR_PHOSPHOR_BRIGHT, "KEYS: %s", shortcuts);
        }

        Terminal_PrintRow(bottomRow, COLOR_PHOSPHOR_DIM, "    %s", shortcuts2);
    } else {
        Terminal_DrawHLine(bottomRow - 2, '-', COLOR_PHOSPHOR_DARK);

        if (statusMsg && statusMsg[0]) {
            Terminal_PrintRow(bottomRow - 1, COLOR_PHOSPHOR_AMBER, "STATUS: %s", statusMsg);
        } else {
            Terminal_PrintRow(bottomRow - 1, COLOR_PHOSPHOR_DIM, "STATUS: Ready (SMBus @ 0xD2 Active)");
        }

        if (shortcuts && shortcuts[0]) {
            Terminal_PrintRow(bottomRow, COLOR_PHOSPHOR_BRIGHT, "KEYS: %s", shortcuts);
        }
    }
}

typedef struct {
    float x, y, z, rhw;
    DWORD color;
} D3D_VERTEX_2D;

#define D3DFVF_VERTEX_2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
// Batches must not exceed 2047 / sizeof(D3D_VERTEX_2D) = 102 vertices to prevent
// DrawVerticesUP from splitting across push-buffer packets.
#define LINE_BATCH_SIZE 64
#define QUAD_BATCH_SIZE 64

static D3D_VERTEX_2D s_lineBatch[LINE_BATCH_SIZE];
static int s_lineVertexCount = 0;

static D3D_VERTEX_2D s_quadBatch[QUAD_BATCH_SIZE];
static int s_quadVertexCount = 0;

static void setup_2d_pipeline_states(void)
{
    D3DFont_InvalidateState();
    D3DDevice_SetVertexShader(D3DFVF_VERTEX_2D);
    D3DDevice_SetPixelShader(0);
    D3DDevice_SetRenderState(D3DRS_LIGHTING, FALSE);
    D3DDevice_SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    D3DDevice_SetTexture(0, NULL);
    D3DDevice_SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    D3DDevice_SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
}

void Terminal_FlushBatch(void)
{
    if (!g_pDevice) return;

    if (s_lineVertexCount > 0) {
        setup_2d_pipeline_states();
        D3DDevice_DrawVerticesUP(D3DPT_LINELIST, s_lineVertexCount, s_lineBatch, sizeof(D3D_VERTEX_2D));
        s_lineVertexCount = 0;
    }

    if (s_quadVertexCount > 0) {
        setup_2d_pipeline_states();
        D3DDevice_DrawVerticesUP(D3DPT_QUADLIST, s_quadVertexCount, s_quadBatch, sizeof(D3D_VERTEX_2D));
        s_quadVertexCount = 0;
    }
}

void Terminal_DrawLine(float x1, float y1, float x2, float y2, DWORD color)
{
    if (!g_pDevice) return;
    if (s_lineVertexCount + 2 > LINE_BATCH_SIZE) {
        Terminal_FlushBatch();
    }
    s_lineBatch[s_lineVertexCount + 0] = (D3D_VERTEX_2D){ x1, y1, 0.0f, 1.0f, color };
    s_lineBatch[s_lineVertexCount + 1] = (D3D_VERTEX_2D){ x2, y2, 0.0f, 1.0f, color };
    s_lineVertexCount += 2;
}

void Terminal_DrawLineThick(float x1, float y1, float x2, float y2, DWORD color)
{
    Terminal_DrawLine(x1, y1, x2, y2, color);
    if (y1 == y2) {
        Terminal_DrawLine(x1, y1 + 1.0f, x2, y2 + 1.0f, color);
    } else if (x1 == x2) {
        Terminal_DrawLine(x1 + 1.0f, y1, x2 + 1.0f, y2, color);
    } else {
        Terminal_DrawLine(x1 + 1.0f, y1, x2 + 1.0f, y2, color);
        Terminal_DrawLine(x1, y1 + 1.0f, x2, y2 + 1.0f, color);
    }
}

void Terminal_DrawRect(float x, float y, float w, float h, DWORD color)
{
    float r = x + w;
    float b = y + h;
    Terminal_DrawLine(x, y, r, y, color);
    Terminal_DrawLine(r, y, r, b, color);
    Terminal_DrawLine(r, b, x, b, color);
    Terminal_DrawLine(x, b, x, y, color);
}

void Terminal_DrawRectThick(float x, float y, float w, float h, DWORD color)
{
    Terminal_DrawRect(x, y, w, h, color);
    if (w > 2.0f && h > 2.0f) {
        Terminal_DrawRect(x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, color);
    }
}

void Terminal_DrawFilledRect(float x, float y, float w, float h, DWORD color)
{
    if (!g_pDevice) return;
    if (s_quadVertexCount + 4 > QUAD_BATCH_SIZE) {
        Terminal_FlushBatch();
    }

    float r = x + w;
    float b = y + h;

    s_quadBatch[s_quadVertexCount + 0] = (D3D_VERTEX_2D){ x, b, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 1] = (D3D_VERTEX_2D){ x, y, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 2] = (D3D_VERTEX_2D){ r, y, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 3] = (D3D_VERTEX_2D){ r, b, 0.0f, 1.0f, color };
    s_quadVertexCount += 4;
}

void Terminal_DrawAlignmentReticle(int pos_h, int pos_v)
{
    float cx = (float)g_screenWidth / 2.0f;  // Exactly 640.0f in 720p
    float cy = (float)g_screenHeight / 2.0f; // Exactly 360.0f in 720p
    float sw = (float)g_screenWidth;
    float sh = (float)g_screenHeight;

    // 1. Outer 720p Active Display Bounds (1280x720 100% raster edge) - double width
    Terminal_DrawRectThick(2.0f, 2.0f, sw - 4.0f, sh - 4.0f, COLOR_PHOSPHOR_DARK);

    // Corner brackets at screen perimeter (24px each) - double width
    // Top-Left
    Terminal_DrawLineThick(2.0f, 24.0f, 24.0f, 24.0f, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLineThick(24.0f, 2.0f, 24.0f, 24.0f, COLOR_PHOSPHOR_DIM);
    // Top-Right
    Terminal_DrawLineThick(sw - 24.0f, 24.0f, sw - 2.0f, 24.0f, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLineThick(sw - 24.0f, 2.0f, sw - 24.0f, 24.0f, COLOR_PHOSPHOR_DIM);
    // Bottom-Left
    Terminal_DrawLineThick(2.0f, sh - 24.0f, 24.0f, sh - 24.0f, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLineThick(24.0f, sh - 2.0f, 24.0f, sh - 24.0f, COLOR_PHOSPHOR_DIM);
    // Bottom-Right
    Terminal_DrawLineThick(sw - 24.0f, sh - 24.0f, sw - 2.0f, sh - 24.0f, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLineThick(sw - 24.0f, sh - 2.0f, sw - 24.0f, sh - 24.0f, COLOR_PHOSPHOR_DIM);

    // 2. Action Safe Boundary (5% inset)
    float safeH = sw * 0.05f;
    float safeV = sh * 0.05f;
    Terminal_DrawRectThick(safeH, safeV, sw - 2.0f * safeH, sh - 2.0f * safeV, COLOR_PHOSPHOR_DIM);

    // 3. Main Centering Reticle Box (Square alignment target)
    float boxSize = (sh >= 720.0f) ? 220.0f : 140.0f;
    float boxW = boxSize;
    float boxH = boxSize;
    float boxL = cx - boxW / 2.0f;
    float boxT = cy - boxH / 2.0f;
    Terminal_DrawRect(boxL, boxT, boxW, boxH, COLOR_PHOSPHOR_DARK);

    // Inner Target Frame (Square)
    float inSize = boxSize * 0.40f;
    Terminal_DrawRect(cx - inSize / 2.0f, cy - inSize / 2.0f, inSize, inSize, COLOR_PHOSPHOR_DIM);

    // 4. Center Crosshairs passing through EXACT (cx, cy)
    Terminal_DrawLine(boxL, cy, boxL + boxW, cy, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLine(cx, boxT, cx, boxT + boxH, COLOR_PHOSPHOR_DIM);

    // Precision Center Cross (bright center marker) - double width
    Terminal_DrawLineThick(cx - 24.0f, cy, cx + 24.0f, cy, COLOR_PHOSPHOR_BRIGHT);
    Terminal_DrawLineThick(cx, cy - 24.0f, cx, cy + 24.0f, COLOR_PHOSPHOR_BRIGHT);
    // Center Bullseye Box (12x12 px)
    Terminal_DrawRect(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, COLOR_PHOSPHOR_BRIGHT);

    // 5. Tick marks along the axes (every 20 px)
    int maxTick = (int)(boxSize / 2.0f) - 15;
    for (int t = -maxTick; t <= maxTick; t += 20) {
        if (t == 0) continue;
        float tx = cx + (float)t;
        float tickH = (t % 100 == 0) ? 14.0f : ((t % 40 == 0) ? 10.0f : 6.0f);
        DWORD col = (t % 100 == 0) ? COLOR_PHOSPHOR_NORMAL : COLOR_PHOSPHOR_DIM;
        Terminal_DrawLine(tx, cy - tickH / 2.0f, tx, cy + tickH / 2.0f, col);
    }
    for (int t = -maxTick; t <= maxTick; t += 20) {
        if (t == 0) continue;
        float ty = cy + (float)t;
        float tickW = (t % 100 == 0) ? 14.0f : ((t % 40 == 0) ? 10.0f : 6.0f);
        DWORD col = (t % 100 == 0) ? COLOR_PHOSPHOR_NORMAL : COLOR_PHOSPHOR_DIM;
        Terminal_DrawLine(cx - tickW / 2.0f, ty, cx + tickW / 2.0f, ty, col);
    }

    // 6. Centered Legend Text directly ABOVE the center reticle box
    int rCenter = (int)((boxT - (float)g_lineHeight - (float)g_marginTop + 4.0f) / (float)g_lineHeight);
    if (rCenter < 5) rCenter = 5;

    if (sh <= 480.0f) {
        Terminal_PrintRowCenter(rCenter, COLOR_PHOSPHOR_NORMAL, "[ SCREEN CENTER : X=%d, Y=%d ]", (int)cx, (int)cy);
    } else {
        Terminal_PrintRowCenter(rCenter, COLOR_PHOSPHOR_NORMAL, "[ SCREEN CENTER : X=%d, Y=%d ]", (int)cx, (int)cy);
    }
}

void UI_RunCRTCollapseAndExit(void)
{
    if (!g_pDevice) {
        XLaunchNewImage(NULL, NULL);
        return;
    }

    int width = g_screenWidth;
    int height = g_screenHeight;
    float halfWidth = (float)width / 2.0f;
    float halfHeight = (float)height / 2.0f;

    // Phase 1: Pure black screen with solitary string "END OF LINE."
    // Hold steady for 30 frames (~500 ms at 60 FPS)
    for (int frame = 0; frame < 30; frame++) {
        Terminal_BeginFrame();
        Terminal_PrintCentered(COLOR_PHOSPHOR_BRIGHT, "END OF LINE.");
        Terminal_EndFrame();
    }

    // Phase 2: Collapsing CRT raster - black bars from top and bottom meet in the middle
    // Leading edges exhibit an intense electron beam compression flare across the screen.
    // 36 frames (~600 ms at 60 FPS)
    const int kCollapseFrames = 36;
    for (int frame = 0; frame <= kCollapseFrames; frame++) {
        float t = (float)frame / (float)kCollapseFrames;
        // Ease-in quadratic curve simulates magnetic deflection collapse
        float barHeight = t * t * halfHeight;
        if (barHeight > halfHeight) barHeight = halfHeight;

        Terminal_BeginFrame();
        // Print the string first
        Terminal_PrintCentered(COLOR_PHOSPHOR_BRIGHT, "END OF LINE.");

        // Top bar closing down
        if (barHeight > 0.0f) {
            Terminal_DrawFilledRect(0.0f, 0.0f, (float)width, barHeight, D3DCOLOR_XRGB(0, 0, 0));
            // Electron beam compression glow along bottom edge of top bar
            Terminal_DrawFilledRect(0.0f, barHeight - 1.0f, (float)width, 3.0f, D3DCOLOR_XRGB(120, 255, 120));
            Terminal_DrawLine(0.0f, barHeight, (float)width, barHeight, D3DCOLOR_XRGB(240, 255, 240));
        }

        // Bottom bar closing up
        if (barHeight > 0.0f) {
            float bottomY = (float)height - barHeight;
            Terminal_DrawFilledRect(0.0f, bottomY, (float)width, barHeight, D3DCOLOR_XRGB(0, 0, 0));
            // Electron beam compression glow along top edge of bottom bar
            Terminal_DrawFilledRect(0.0f, bottomY - 1.0f, (float)width, 3.0f, D3DCOLOR_XRGB(120, 255, 120));
            Terminal_DrawLine(0.0f, bottomY, (float)width, bottomY, D3DCOLOR_XRGB(240, 255, 240));
        }

        Terminal_EndFrame();
    }

    // Phase 3: Vertical raster has collapsed into the center line.
    // Now the horizontal beam pinches inwards from both sides towards the center dot.
    // 22 frames (~366 ms at 60 FPS)
    const int kPinchFrames = 22;
    for (int frame = 0; frame <= kPinchFrames; frame++) {
        float t = (float)frame / (float)kPinchFrames;
        float eased = 1.0f - (t * t);
        float span = eased * halfWidth;
        float x1 = halfWidth - span;
        float x2 = halfWidth + span;

        Terminal_BeginFrame();
        if (span > 1.0f) {
            Terminal_DrawFilledRect(x1, halfHeight - 1.0f, span * 2.0f, 3.0f, D3DCOLOR_XRGB(120, 255, 120));
            Terminal_DrawLine(x1, halfHeight, x2, halfHeight, D3DCOLOR_XRGB(255, 255, 255));
        }
        Terminal_EndFrame();
    }

    // Phase 4: Center phosphor dot glowing and fading out
    // 12 frames (~200 ms at 60 FPS)
    const int kDotFrames = 12;
    for (int frame = 0; frame < kDotFrames; frame++) {
        float t = (float)frame / (float)kDotFrames;
        int intensity = (int)(255.0f * (1.0f - t));
        if (intensity < 0) intensity = 0;
        int green = (int)(255.0f * (1.0f - t * 0.7f));

        Terminal_BeginFrame();
        // Soft outer glow
        Terminal_DrawFilledRect(halfWidth - 3.0f, halfHeight - 2.0f, 6.0f, 4.0f,
                               D3DCOLOR_XRGB(intensity / 3, green / 2, intensity / 3));
        // Bright core
        Terminal_DrawFilledRect(halfWidth - 1.5f, halfHeight - 1.0f, 3.0f, 2.0f,
                               D3DCOLOR_XRGB(intensity, green, intensity));
        Terminal_EndFrame();
    }

    // Phase 5: Final pure black frame
    Terminal_BeginFrame();
    Terminal_EndFrame();

    // Relaunch the Xbox default dashboard (stock or custom)
    XLaunchNewImage(NULL, NULL);

    while (1) {
        Sleep(1000);
    }
}


