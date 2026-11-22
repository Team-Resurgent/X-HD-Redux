// SPDX-License-Identifier: GPL-3.0-only
#include "ui_picklist.h"
#include "ui_terminal.h"

#define PAD_PX 10

static bool s_active;
static int  s_id;
static const char *s_title;
static const char *s_items[PICK_MAX_ITEMS];
static int  s_count;
static int  s_current;   // the value in effect, marked in the list
static int  s_cursor;
static int  s_top;       // first item shown (the list scrolls when it doesn't fit)
static int  s_row;
static int  s_indent;

void Picklist_Open(int id, const char *title, const char *const *items, int count, int current,
                   int row, int indent_px)
{
    if (count <= 0) return;
    if (count > PICK_MAX_ITEMS) count = PICK_MAX_ITEMS;
    for (int i = 0; i < count; i++) s_items[i] = items[i];
    s_id = id;
    s_title = title;
    s_count = count;
    s_current = (current >= 0 && current < count) ? current : -1;
    s_cursor = s_current >= 0 ? s_current : 0;
    s_top = 0;
    s_row = row;
    s_indent = indent_px;
    s_active = true;
}

bool Picklist_IsActive(void) { return s_active; }
int  Picklist_Id(void) { return s_id; }

pick_result_t Picklist_HandleInput(const input_state_t *in, int *chosen)
{
    if (!s_active) return PICK_NONE;
    if (in->up && s_cursor > 0) s_cursor--;
    if (in->down && s_cursor < s_count - 1) s_cursor++;
    if (in->a) {
        s_active = false;
        *chosen = s_cursor;
        return PICK_CHOSEN;
    }
    if (in->b || in->back) {
        s_active = false;
        return PICK_CANCELLED;
    }
    return PICK_NONE;
}

void Picklist_Render(void)
{
    if (!s_active) return;

    const int lh = Terminal_GetLineHeight();
    const int mt = Terminal_GetTopMargin();
    const int ml = Terminal_GetLeftMargin();

    // Title row plus as many items as fit above the footer.
    const int last = Terminal_FooterTopRow() - 1;
    int top_row = s_row;
    if (top_row + s_count > last) top_row = last - s_count;
    if (top_row < 5) top_row = 5;
    int shown = last - top_row;
    if (shown > s_count) shown = s_count;
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + shown) s_top = s_cursor - shown + 1;

    int w = Terminal_TextWidth(s_title);
    for (int i = 0; i < s_count; i++) {
        int iw = Terminal_TextWidth("> ") + Terminal_TextWidth(s_items[i]) + Terminal_TextWidth("  *");
        if (iw > w) w = iw;
    }
    int indent = s_indent;
    const int room = Terminal_GetWidth() - 2 * ml - 2 * PAD_PX;
    if (indent + w > room) indent = room - w;
    if (indent < PAD_PX) indent = PAD_PX;

    const float x = (float)(ml + indent - PAD_PX);
    const float y = (float)(mt + top_row * lh - 6);
    const float bw = (float)(w + 2 * PAD_PX);
    const float bh = (float)((shown + 1) * lh + 10);
    Terminal_DrawFilledRect(x, y, bw, bh, D3DCOLOR_XRGB(0, 0, 0));
    Terminal_DrawRectThick(x, y, bw, bh, COLOR_PHOSPHOR_DIM);
    Terminal_DrawLine(x, y + (float)lh + 2.0f, x + bw, y + (float)lh + 2.0f, COLOR_PHOSPHOR_DARK);

    Terminal_PrintRowIndent(top_row, indent, COLOR_PHOSPHOR_DIM, "%s%s", s_title,
                            shown < s_count ? "  (more)" : "");
    for (int i = 0; i < shown; i++) {
        const int it = s_top + i;
        const bool sel = it == s_cursor;
        Terminal_PrintRowIndent(top_row + 1 + i, indent, sel ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                                "%s%s%s", sel ? "> " : "  ", s_items[it], it == s_current ? "  *" : "");
    }
}
