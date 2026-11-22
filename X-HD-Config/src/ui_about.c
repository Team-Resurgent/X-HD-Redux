// SPDX-License-Identifier: GPL-3.0-only
// The Help tab: sections (help_text.c) of items; each item is a name and a one-line summary,
// and (A) opens its full explanation underneath, wrapped to the screen width. (What is running
// -- app, firmware, board -- is on the Status tab.)
#include "ui_about.h"
#include "ui_terminal.h"
#include "help_text.h"
#include <stdio.h>
#include <string.h>

// Row 5: heading; rows 6 .. above the footer: help.
#define HELP_FIRST_ROW  6
#define MAX_SECTIONS    16
#define MAX_ITEMS       16
#define MAX_LINES       640

// Layout, in 1280x720 pixels from the left margin.
#define ITEM_X          28    // an item's "> [+] Name"
#define BRIEF_GAP       24    // between the widest item name and the summary column
#define DETAIL_X        72    // an open item's explanation

typedef enum { L_SECTION, L_ITEM, L_DETAIL, L_GAP } line_kind_t;

typedef struct {
    uint8_t kind;           // line_kind_t
    uint8_t sec;
    int8_t  item;           // -1 on a section line
    const char *text;       // L_DETAIL: the wrapped piece
    uint16_t len;
} help_line_t;

static bool s_sec_open[MAX_SECTIONS];
static bool s_item_open[MAX_SECTIONS][MAX_ITEMS];
static int  s_cur_sec;
static int  s_cur_item = -1;     // -1: the section line itself
static int  s_scroll;            // first help line shown
static bool s_follow;            // the cursor moved: keep it on screen
static bool s_reveal;            // an item just opened: bring its text into view
static int  s_manual;            // lines scrolled by the triggers / stick since the last frame

static help_line_t s_lines[MAX_LINES];
static int s_line_count;

static int item_count(int sec)
{
    int n = 0;
    while (kHelpSections[sec].items[n].name && n < MAX_ITEMS) n++;
    return n;
}

static int section_count(void)
{
    return kHelpSectionCount < MAX_SECTIONS ? kHelpSectionCount : MAX_SECTIONS;
}

static int help_rows(void)
{
    return Terminal_FooterTopRow() - HELP_FIRST_ROW;
}

void About_Init(void)
{
    memset(s_sec_open, 0, sizeof(s_sec_open));
    memset(s_item_open, 0, sizeof(s_item_open));
    s_sec_open[0] = true;   // the first section (Controls), open on arrival
    s_cur_sec = 0;
    s_cur_item = -1;
    s_scroll = 0;
}

// ---------------------------------------------------------------------------- layout

static void add_line(line_kind_t kind, int sec, int item, const char *text, int len)
{
    if (s_line_count >= MAX_LINES) return;
    help_line_t *l = &s_lines[s_line_count++];
    l->kind = (uint8_t)kind;
    l->sec = (uint8_t)sec;
    l->item = (int8_t)item;
    l->text = text;
    l->len = (uint16_t)len;
}

static int text_width(const char *s, int len)
{
    char buf[256];
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    return Terminal_TextWidth(buf);
}

// Word-wraps `text` into lines no wider than `width` px; '\n' ends a paragraph, with a blank
// line between paragraphs.
static void add_wrapped(int sec, int item, const char *text, int width)
{
    const char *p = text;
    while (*p) {
        const char *para_end = strchr(p, '\n');
        if (!para_end) para_end = p + strlen(p);
        while (p < para_end) {
            while (p < para_end && *p == ' ') p++;
            const char *fit = p;       // end of the longest run of whole words that fits
            const char *q = p;
            while (q < para_end) {
                const char *w = q;
                while (w < para_end && *w == ' ') w++;
                while (w < para_end && *w != ' ') w++;
                if (text_width(p, (int)(w - p)) > width && fit > p) break;
                fit = w;
                q = w;
            }
            if (fit == p) fit = para_end;   // one word wider than the line: let it be cut
            add_line(L_DETAIL, sec, item, p, (int)(fit - p));
            p = fit;
        }
        if (*para_end == '\n') {
            add_line(L_GAP, sec, item, NULL, 0);
            p = para_end + 1;
        } else {
            p = para_end;
        }
    }
}

static void build_lines(void)
{
    s_line_count = 0;
    const int width = Terminal_GetWidth() - 2 * Terminal_GetLeftMargin() - DETAIL_X;
    for (int s = 0; s < section_count(); s++) {
        add_line(L_SECTION, s, -1, NULL, 0);
        if (!s_sec_open[s]) continue;
        const int n = item_count(s);
        for (int i = 0; i < n; i++) {
            add_line(L_ITEM, s, i, NULL, 0);
            if (s_item_open[s][i]) {
                add_wrapped(s, i, kHelpSections[s].items[i].detail, width);
                add_line(L_GAP, s, i, NULL, 0);
            }
        }
        add_line(L_GAP, s, -1, NULL, 0);
    }
}

// The summary column: just past the widest "> [+] Name", measured in the real font.
static int brief_x(void)
{
    static int x;
    if (x == 0) {
        for (int s = 0; s < section_count(); s++) {
            for (int i = 0; i < item_count(s); i++) {
                char buf[64];
                snprintf(buf, sizeof(buf), "> [+] %s", kHelpSections[s].items[i].name);
                const int w = ITEM_X + Terminal_TextWidth(buf) + BRIEF_GAP;
                if (w > x) x = w;
            }
        }
    }
    return x;
}

static bool selectable(const help_line_t *l)
{
    return l->kind == L_SECTION || l->kind == L_ITEM;
}

static int cursor_line(void)
{
    for (int i = 0; i < s_line_count; i++) {
        if (selectable(&s_lines[i]) && s_lines[i].sec == s_cur_sec && s_lines[i].item == s_cur_item) {
            return i;
        }
    }
    return 0;
}

static void set_cursor(const help_line_t *l)
{
    s_cur_sec = l->sec;
    s_cur_item = l->item;
}

// ---------------------------------------------------------------------------- input

void About_HandleInput(const input_state_t *in)
{
    if (!in) return;
    build_lines();
    const int cur = cursor_line();

    if (in->up) {
        for (int i = cur - 1; i >= 0; i--) {
            if (selectable(&s_lines[i])) { set_cursor(&s_lines[i]); break; }
        }
        s_follow = true;
    }
    if (in->down) {
        for (int i = cur + 1; i < s_line_count; i++) {
            if (selectable(&s_lines[i])) { set_cursor(&s_lines[i]); break; }
        }
        s_follow = true;
    }
    if (in->a) {
        if (s_cur_item < 0) {
            s_sec_open[s_cur_sec] = !s_sec_open[s_cur_sec];
        } else {
            bool *open = &s_item_open[s_cur_sec][s_cur_item];
            *open = !*open;
            s_reveal = *open;
        }
        s_follow = true;
    }
    // (B) closes one level: the open item under the cursor, else its section.
    if (in->b || in->back) {
        if (s_cur_item >= 0 && s_item_open[s_cur_sec][s_cur_item]) {
            s_item_open[s_cur_sec][s_cur_item] = false;
        } else {
            s_sec_open[s_cur_sec] = false;
            s_cur_item = -1;
        }
        s_follow = true;
    }
    // (X) opens every section, or closes them all (and their items) when all are open.
    if (in->x) {
        bool any_closed = false;
        for (int s = 0; s < section_count(); s++) any_closed |= !s_sec_open[s];
        for (int s = 0; s < section_count(); s++) s_sec_open[s] = any_closed;
        if (!any_closed) {
            memset(s_item_open, 0, sizeof(s_item_open));
            s_cur_item = -1;
        }
        s_follow = true;
    }

    if (in->l_trigger) s_manual -= 3;
    if (in->r_trigger) s_manual += 3;
    if (in->thumb_ly > 16000) s_manual -= 1;
    if (in->thumb_ly < -16000) s_manual += 1;
}

// ---------------------------------------------------------------------------- render

static void scroll_into_view(void)
{
    const int rows = help_rows();
    const int cur = cursor_line();

    if (s_manual) {
        s_scroll += s_manual;
        s_manual = 0;
        // Scrolled away from the cursor: take it along to the nearest item on screen.
        if (cur < s_scroll || cur >= s_scroll + rows) {
            const int top = s_scroll < 0 ? 0 : s_scroll;
            for (int i = top; i < s_line_count && i < top + rows; i++) {
                if (selectable(&s_lines[i])) { set_cursor(&s_lines[i]); break; }
            }
        }
    }
    if (s_reveal) {
        // The item's last line of text into view, the item itself staying on screen.
        int last = cur;
        while (last + 1 < s_line_count && s_lines[last + 1].kind != L_SECTION &&
               s_lines[last + 1].kind != L_ITEM) {
            last++;
        }
        if (last >= s_scroll + rows) s_scroll = last - rows + 1;
        if (s_scroll > cur) s_scroll = cur;
        s_reveal = false;
    }
    if (s_follow) {
        const int c = cursor_line();
        if (c < s_scroll) s_scroll = c;
        if (c >= s_scroll + rows) s_scroll = c - rows + 1;
        s_follow = false;
    }
    const int max_scroll = s_line_count > rows ? s_line_count - rows : 0;
    if (s_scroll > max_scroll) s_scroll = max_scroll;
    if (s_scroll < 0) s_scroll = 0;
}

static void render_line(int row, const help_line_t *l, bool at_cursor)
{
    const help_section_t *sec = &kHelpSections[l->sec];
    const char *mark = at_cursor ? ">" : " ";
    switch (l->kind) {
    case L_SECTION:
        Terminal_PrintRow(row, at_cursor ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_ACCENT, "%s [%c] %s",
                          mark, s_sec_open[l->sec] ? '-' : '+', sec->title);
        break;
    case L_ITEM: {
        const help_item_t *it = &sec->items[l->item];
        Terminal_PrintRowIndent(row, ITEM_X, at_cursor ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_NORMAL,
                                "%s [%c] %s", mark, s_item_open[l->sec][l->item] ? '-' : '+', it->name);
        Terminal_PrintRowIndent(row, brief_x(), at_cursor ? COLOR_PHOSPHOR_NORMAL : COLOR_PHOSPHOR_DIM,
                                "%s", it->brief);
        break;
    }
    case L_DETAIL:
        Terminal_PrintRowIndent(row, DETAIL_X, COLOR_PHOSPHOR_NORMAL, "%.*s", (int)l->len, l->text);
        break;
    default:
        break;
    }
}

void About_Render(void)
{
    build_lines();
    scroll_into_view();
    const int rows = help_rows();
    Terminal_PrintRow(HELP_FIRST_ROW - 1, COLOR_PHOSPHOR_DIM, "HELP:  (A) opens an item, (B) closes it");
    if (s_line_count > rows) {
        Terminal_PrintRowRight(HELP_FIRST_ROW - 1, COLOR_PHOSPHOR_DIM, "[ TRIGGERS: SCROLL  %d-%d of %d ]",
                               s_scroll + 1, s_scroll + rows < s_line_count ? s_scroll + rows : s_line_count,
                               s_line_count);
    }

    const int cur = cursor_line();
    for (int r = 0; r < rows && s_scroll + r < s_line_count; r++) {
        render_line(HELP_FIRST_ROW + r, &s_lines[s_scroll + r], s_scroll + r == cur);
    }
}
