// SPDX-License-Identifier: GPL-3.0-only
#include "ui_terminal.h"
#include "d3d_font.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static D3DDevice  *g_pDevice = NULL;

// Every screen is laid out for 1280x720; the vertices are scaled to the back buffer as they are
// written (0.5625 x 0.667 at 720x480, which is right for its 16:9 pixels; 1.5 at 1080i).
static const int g_screenWidth  = 1280;
static const int g_screenHeight = 720;
static const int g_marginLeft   = 80;
static const int g_marginRight  = 80;
static const int g_marginTop    = 36;
static const int g_lineHeight   = 26;
static float g_outScaleX = 1.0f;
static float g_outScaleY = 1.0f;

int Terminal_GetWidth(void)      { return g_screenWidth; }
int Terminal_GetHeight(void)     { return g_screenHeight; }
int Terminal_GetLineHeight(void) { return g_lineHeight; }
int Terminal_GetTopMargin(void)  { return g_marginTop; }
int Terminal_GetLeftMargin(void) { return g_marginLeft; }

static void set_output_size(int width, int height)
{
    g_outScaleX = (float)width / (float)g_screenWidth;
    g_outScaleY = (float)height / (float)g_screenHeight;
    D3DFont_SetOutputScale(g_outScaleX, g_outScaleY);

    D3DVIEWPORT8 vp;
    ZeroMemory(&vp, sizeof(vp));
    vp.Width = (DWORD)width;
    vp.Height = (DWORD)height;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    D3DDevice_SetViewport(&vp);
}

static void fill_present_params(D3DPRESENT_PARAMETERS *pp, int width, int height, DWORD d3d_flags, UINT refreshRate)
{
    ZeroMemory(pp, sizeof(*pp));
    pp->BackBufferWidth = width;
    pp->BackBufferHeight = height;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferCount = 1;
    pp->Windowed = FALSE;
    pp->EnableAutoDepthStencil = FALSE;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->FullScreen_RefreshRateInHz = (refreshRate > 0) ? refreshRate : 60;
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp->Flags = d3d_flags;
}

bool Terminal_Init(int width, int height, DWORD d3d_flags, UINT refreshRate)
{
    static Direct3D *pD3D;
    if (!pD3D) {
        pD3D = Direct3DCreate8(D3D_SDK_VERSION);
        if (!pD3D) {
            return false;
        }
    }

    D3DPRESENT_PARAMETERS pp;
    fill_present_params(&pp, width, height, d3d_flags, refreshRate);
    if (FAILED(Direct3D_CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_pDevice))) {
        g_pDevice = NULL;
        return false;
    }
    set_output_size(width, height);

    // Initialize D3D textured quad font renderer (Arial proportional atlas)
    return D3DFont_Init();
}

bool Terminal_SetVideoMode(int width, int height, DWORD d3d_flags, UINT refreshRate)
{
    if (!g_pDevice) return false;

    // Unbind texture and shader state before device reset
    D3DDevice_SetTexture(0, NULL);
    D3DDevice_SetVertexShader(0);
    D3DDevice_SetPixelShader(0);

    D3DPRESENT_PARAMETERS pp;
    fill_present_params(&pp, width, height, d3d_flags, refreshRate);
    if (FAILED(D3DDevice_Reset(&pp))) {
        return false;
    }
    set_output_size(width, height);

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

int Terminal_TextWidth(const char *text)
{
    WCHAR wbuf[256];
    int len = 0;
    while (text[len] && len < 255) {
        wbuf[len] = (WCHAR)(unsigned char)text[len];
        len++;
    }
    wbuf[len] = 0;
    float w = 0.0f;
    D3DFont_GetTextExtent(wbuf, len, &w, NULL);
    return (int)w;
}

typedef enum { AL_LEFT, AL_RIGHT, AL_CENTER } align_t;

static int row_y(int row) { return g_marginTop + row * g_lineHeight; }

// Formats one line and draws it at y, placed by `align` inside [left, left + width).
static void print_v(int y, int left, int width, align_t align, DWORD color, const char *fmt, va_list args)
{
    char buf[512];
    WCHAR wbuf[512];
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    if (len <= 0) return;
    int x = left;
    if (align != AL_LEFT) {
        float w = 0.0f;
        D3DFont_GetTextExtent(wbuf, len, &w, NULL);
        x = (align == AL_RIGHT) ? left + width - (int)w : left + (width - (int)w) / 2;
        if (x < g_marginLeft) x = g_marginLeft;
    }
    clamp_and_render_text(x, y, color, wbuf, len);
}

void Terminal_PrintAt(int x, int y, DWORD color, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    print_v(y, x, 0, AL_LEFT, color, fmt, args);
    va_end(args);
}

void Terminal_PrintRow(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    va_list args;
    va_start(args, fmt);
    print_v(row_y(row), g_marginLeft, 0, AL_LEFT, color, fmt, args);
    va_end(args);
}

// Cell for monospace rows: the widest glyph a hex dump prints. Wider characters in
// the ASCII column are still centered on their cell and may touch their neighbors.
static float mono_cell_width(void)
{
    static float cell = 0.0f;
    if (cell <= 0.0f) {
        static const WCHAR set[] = L"0123456789ABCDEFx:-";
        for (int i = 0; set[i] != 0; i++) {
            float a = D3DFont_GetGlyphAdvance(set[i]);
            if (a > cell) cell = a;
        }
    }
    return cell;
}

void Terminal_PrintRowMono(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    char buf[512];
    WCHAR wbuf[512];
    va_list args;
    va_start(args, fmt);
    int len = format_string(buf, sizeof(buf), wbuf, sizeof(wbuf) / sizeof(WCHAR), fmt, args);
    va_end(args);
    if (len <= 0) return;

    int y = row_y(row);
    if (y < 0 || y + g_lineHeight > g_screenHeight) return;

    float cell = mono_cell_width();
    if (cell <= 0.0f) return;

    // Truncate to the drawable width between the margins
    int maxChars = (int)((float)(g_screenWidth - g_marginLeft - g_marginRight) / cell);
    if (len > maxChars) {
        len = maxChars;
        wbuf[len] = 0;
    }
    if (len <= 0) return;

    Terminal_FlushBatch();
    D3DFont_DrawTextMono((float)g_marginLeft, (float)y, color, wbuf, len, cell);
}

void Terminal_PrintRowIndent(int row, int indentPx, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    va_list args;
    va_start(args, fmt);
    print_v(row_y(row), g_marginLeft + indentPx, 0, AL_LEFT, color, fmt, args);
    va_end(args);
}

void Terminal_PrintRowRight(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    va_list args;
    va_start(args, fmt);
    print_v(row_y(row), g_marginLeft, g_screenWidth - g_marginLeft - g_marginRight, AL_RIGHT, color, fmt, args);
    va_end(args);
}

void Terminal_PrintRowCenter(int row, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    va_list args;
    va_start(args, fmt);
    print_v(row_y(row), 0, g_screenWidth, AL_CENTER, color, fmt, args);
    va_end(args);
}

void Terminal_PrintRowCenterIn(int row, int leftIndentPx, int widthPx, DWORD color, const char *fmt, ...)
{
    if (row < 0) return;
    va_list args;
    va_start(args, fmt);
    print_v(row_y(row), g_marginLeft + leftIndentPx, widthPx, AL_CENTER, color, fmt, args);
    va_end(args);
}

void Terminal_PrintItem(int row, bool selected, const char *fmt, ...)
{
    if (row < 0) return;
    char body[480];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';
    Terminal_PrintRow(row, selected ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                      "%s%s", selected ? "> " : "  ", body);
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

void Terminal_DrawHeader(const terminal_header_t *h)
{
    // Double horizontal border
    Terminal_DrawHLine(0, '=', COLOR_PHOSPHOR_ACCENT);

    // Title and right-aligned board state
    const char *title = "X-HD REDUX SYSTEM CONFIGURATION TERMINAL";
    Terminal_PrintRow(1, COLOR_PHOSPHOR_BRIGHT, "%s", title);

    // Retro blinking phosphor block cursor at end of heading (~2 Hz)
    if ((GetTickCount() / 450) % 2 == 0) {
        float curX = (float)g_marginLeft + (float)Terminal_TextWidth(title) + 4.0f;
        float curY = (float)(g_marginTop + 1 * g_lineHeight + 3);
        Terminal_DrawFilledRect(curX, curY, 10.0f, (float)g_lineHeight - 6.0f, COLOR_PHOSPHOR_BRIGHT);
    }

    const char *board = h->board_text ? h->board_text : "";
    Terminal_PrintRowRight(1, h->board_color, "%s", board);
    if (h->unsaved) {
        // Just left of the board state.
        static const char kUnsaved[] = "[UNSAVED CHANGES]";
        int x = g_screenWidth - g_marginRight - Terminal_TextWidth(board) - 24 - Terminal_TextWidth(kUnsaved);
        Terminal_PrintAt(x, row_y(1), COLOR_PHOSPHOR_AMBER, "%s", kUnsaved);
    }

    Terminal_DrawHLine(2, '=', COLOR_PHOSPHOR_ACCENT);

    // Tab strip: every tab by number, the active one bracketed with its name; how to move
    // between them on the right.
    int x = g_marginLeft;
    const int gap = Terminal_TextWidth("   ");
    for (int i = 0; i < h->tab_count && i < TERMINAL_MAX_TABS; i++) {
        char item[48];
        if (i == h->active_tab) {
            snprintf(item, sizeof(item), "[%d %s]", i + 1, h->tab_names[i]);
        } else {
            snprintf(item, sizeof(item), "%d", i + 1);
        }
        Terminal_PrintAt(x, row_y(3), i == h->active_tab ? COLOR_PHOSPHOR_ACCENT : COLOR_PHOSPHOR_DIM, "%s", item);
        x += Terminal_TextWidth(item) + gap;
    }
    Terminal_PrintRowRight(3, COLOR_PHOSPHOR_DIM, "Blk\\Wht");

    Terminal_DrawHLine(4, '-', COLOR_PHOSPHOR_DARK);
}

static int footer_bottom_row(void) { return 23; }

int Terminal_FooterTopRow(void)
{
    return footer_bottom_row() - 2;
}

void Terminal_DrawFooter(const char *shortcuts, const char *statusMsg, bool alert)
{
    int bottomRow = footer_bottom_row();

    Terminal_DrawHLine(bottomRow - 2, '-', COLOR_PHOSPHOR_DARK);

    if (statusMsg && statusMsg[0]) {
        Terminal_PrintRow(bottomRow - 1, alert ? COLOR_PHOSPHOR_AMBER : COLOR_PHOSPHOR_NORMAL, "%s", statusMsg);
    }
    if (shortcuts && shortcuts[0]) {
        Terminal_PrintRow(bottomRow, COLOR_PHOSPHOR_BRIGHT, "KEYS: %s", shortcuts);
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
    x1 *= g_outScaleX; x2 *= g_outScaleX;
    y1 *= g_outScaleY; y2 *= g_outScaleY;
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

    float r = (x + w) * g_outScaleX;
    float b = (y + h) * g_outScaleY;
    x *= g_outScaleX;
    y *= g_outScaleY;

    s_quadBatch[s_quadVertexCount + 0] = (D3D_VERTEX_2D){ x, b, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 1] = (D3D_VERTEX_2D){ x, y, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 2] = (D3D_VERTEX_2D){ r, y, 0.0f, 1.0f, color };
    s_quadBatch[s_quadVertexCount + 3] = (D3D_VERTEX_2D){ r, b, 0.0f, 1.0f, color };
    s_quadVertexCount += 4;
}

void Terminal_DrawAlignmentReticle(int pos_h, int pos_v)
{
    float cx = (float)g_screenWidth / 2.0f;
    float cy = (float)g_screenHeight / 2.0f;
    float sw = (float)g_screenWidth;
    float sh = (float)g_screenHeight;

    // 1. Outer active display bounds (100% raster edge) - double width
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
    float boxSize = 220.0f;
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
}

// The exit outro on its own; the caller then leaves (dashboard or console reboot).
void UI_RunCRTCollapse(void)
{
    if (!g_pDevice) {
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
}

void UI_RunCRTCollapseAndExit(void)
{
    UI_RunCRTCollapse();

    // Relaunch the Xbox default dashboard (stock or custom)
    XLaunchNewImage(NULL, NULL);

    while (1) {
        Sleep(1000);
    }
}


