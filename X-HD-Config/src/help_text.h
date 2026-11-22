// SPDX-License-Identifier: GPL-3.0-only
// The help on the About & Help tab: sections in tab order, each a list of items with a
// one-line summary and a longer explanation that (A) opens. Text only; ui_about.c lays it out.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;     // short label, left column
    const char *brief;    // one line, shown beside the name
    const char *detail;   // the full explanation; wrapped to the screen, '\n' starts a paragraph
} help_item_t;

typedef struct {
    const char *title;
    const help_item_t *items;   // ends with a { NULL } item
} help_section_t;

extern const help_section_t kHelpSections[];
extern const int kHelpSectionCount;

#ifdef __cplusplus
}
#endif
