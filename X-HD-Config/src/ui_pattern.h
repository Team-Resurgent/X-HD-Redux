// SPDX-License-Identifier: GPL-3.0-only
// The picture test pattern (Tab 3): near-black steps, large near-black bars, near-white steps
// and colour bars, drawn in framebuffer codes on a code-0 background.
#pragma once

#include "xbox_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Draws the pattern filling the rectangle (pixels in the 1280x720 layout).
void Pattern_Draw(float x, float y, float w, float h);

#ifdef __cplusplus
}
#endif
