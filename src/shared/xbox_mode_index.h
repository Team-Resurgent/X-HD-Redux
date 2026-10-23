#pragma once
// What each console mode_index (an SMBus mode packet's bits 23:16, or the Xcalibur's raster
// registers) is, in one table: its geometry family and the flags the arbiter, the BIOS path and
// standalone classify it by. Pure -- no HAL, no state.

#include <stdbool.h>
#include <stdint.h>

// Geometry family (the arbiter's unit of "same raster shape"). Order is stored state
// (arb_state_t.hw_fam_locked), so don't reorder.
typedef enum {
    FAM_SD_640, FAM_SD_720x480, FAM_SD_720x576, FAM_HD_720p, FAM_HD_1080i, FAM_NONE
} mode_family_t;

#define MODE_FAM_MASK  0x07u
#define MODE_640       0x08u   // 640-wide raster (incl. the 640x576 rows 0x05/0x12)
#define MODE_PAL50     0x10u   // a PAL-table row: 576p50 and the 50 Hz hybrids/placeholders
// Hardware detection is wrong for these, so a BIOS packet wins however fresh a live reading
// looks (arbiter L1): PAL 0x03/0x04/0x10 (the line detector can't classify them) and NTSC
// 0x0F ("1.1:1 PAR" 640x480 -- Focus's width signal reads it as wide, and trusting that
// squeezes the picture into a 720-wide raster).
#define MODE_BIOS_AUTH 0x20u
#define MODE_480P      0x40u   // NTSC 480p (0x07/0x08/0x11): its widescreen bit is unreliable

// Family | flags for mode_index `idx` on `encoder`; FAM_NONE (no flags) for 0 or anything
// past 0x12. 0x07 is 720-wide on Conexant/Focus but a real 640-wide raster on Xcalibur.
uint8_t xbox_mode_flags(uint8_t idx, uint8_t encoder);

static inline bool mode_family_is_hd(mode_family_t f) {
    return f == FAM_HD_720p || f == FAM_HD_1080i;
}

static inline mode_family_t xbox_mode_family(uint8_t idx, uint8_t encoder) {
    return (mode_family_t)(xbox_mode_flags(idx, encoder) & MODE_FAM_MASK);
}
