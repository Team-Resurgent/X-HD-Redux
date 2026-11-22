// SPDX-License-Identifier: GPL-3.0-only
// The HDMI SPD label presets: Tab 1's list and the keyboard's (RT) Next Preset.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SPD_PRESET_COUNT 8

const char *SpdPreset_Vendor(int index);    // up to 8 characters
const char *SpdPreset_Product(int index);   // up to 16 characters
// The 24-byte SPD name: vendor padded to 8, product padded to 16, then a terminator.
void SpdPreset_Name(int index, char out[25]);

#ifdef __cplusplus
}
#endif
