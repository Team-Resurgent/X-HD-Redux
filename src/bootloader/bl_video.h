// SPDX-License-Identifier: GPL-3.0-only
#ifndef __BL_VIDEO_H__
#define __BL_VIDEO_H__

#include <stdint.h>
#include "../shared/types.h"

// Recovery video for the bootloader's "waiting for recovery" mode (enter_bootloader_mode):
// the mode comes from the latched encoder, the pixel clock and the 0x3E line detector
// (bl_video_pick_mode()), its timing from the application's own tables
// (xbox_timing_rows.c, uncalibrated). No SMBus mode packets, EEPROM or arbiter -- that all
// lives in the application. Call once per main-loop iteration.
void bl_video_follow(void);

// How long a mode must hold before it is applied (ms).
#define BL_VIDEO_SETTLE_MS 300u

// The mode_index the recovery screen runs for this clock and 0x3E VIC (0x3E >> 2).
uint8_t bl_video_pick_mode(xbox_encoder enc, uint32_t pclk_hz, uint8_t vic3e);

#endif // __BL_VIDEO_H__
