// SPDX-License-Identifier: GPL-3.0-only
// "Press (A) again to confirm": the first press arms one item, a second press on the same item
// within CONFIRM_TIMEOUT_MS runs it. Past that the confirmation lapses, so a stray (A) minutes
// later can't write the EEPROM or start an update.
#pragma once

#include "xbox_base.h"
#include <stdbool.h>

#define CONFIRM_TIMEOUT_MS 10000

typedef struct {
    int   id;   // the armed item, -1 for none
    DWORD at;   // when it was armed
} confirm_t;

#define CONFIRM_NONE { -1, 0 }

static inline void confirm_arm(confirm_t *c, int id)
{
    c->id = id;
    c->at = GetTickCount();
}

static inline void confirm_clear(confirm_t *c)
{
    c->id = -1;
}

// `id` is armed and still in time.
static inline bool confirm_armed(const confirm_t *c, int id)
{
    return c->id >= 0 && c->id == id && GetTickCount() - c->at < CONFIRM_TIMEOUT_MS;
}

// Something is armed and still in time.
static inline bool confirm_any(const confirm_t *c)
{
    return confirm_armed(c, c->id);
}

// True once, on the frame an armed confirmation runs out (to redraw and say so).
static inline bool confirm_expired(confirm_t *c)
{
    if (c->id >= 0 && GetTickCount() - c->at >= CONFIRM_TIMEOUT_MS) {
        c->id = -1;
        return true;
    }
    return false;
}
