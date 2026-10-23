#include "bl_video.h"

#include "../shared/adv7511.h" // pulls stm32.h (HAL_GetTick)
#include "../shared/adv7511_xbox.h"
#include "../shared/adv7511_vic.h"
#include "../shared/xbox_video_timing.h"
#include "../shared/debug.h"

// Fixed timing constants lifted from the XCALIBUR column of xbox_video_timing.c's
// mode tables (rows 0x01 / 0x02 / 0x0A / 0x0D). On Conexant/Focus the picture sits a
// little off (up to 40 px in HD) -- fine for a recovery screen, and it lets the bootloader drop the whole per-encoder
// table module (xbox_video_timing.c) + the standalone decision machine
// (xbox_video_standalone.c) to make room for the UART updater.
//
// Fields: delay_hs, delay_vs, active_w, active_h, interlaced_offset,
//         sync_adjust_enabled, hs_placement, hs_duration, vs_placement, vs_duration, vic
static const XboxVideoTiming BL_TIMING_SD640 =
    { 96, 35, 640, 480, 0, true,  16, 96, 10, 2, VIC_01_VGA_640x480_4_3 };
static const XboxVideoTiming BL_TIMING_SD720 =
    { 96, 36, 720, 480, 0, true,  16, 62,  9, 6, VIC_02_480p_60__4_3 };
static const XboxVideoTiming BL_TIMING_720p =
    { 260, 25, 1280, 720, 0, false, 0, 0, 0, 0, VIC_04_720p_60_16_9 };
static const XboxVideoTiming BL_TIMING_1080i =
    { 187, 20, 1920, 540, 1, false, 0, 0, 0, 0, VIC_05_1080i_60_16_9 };

// Resolve a raw line-detector VIC (0x3E >> 2) to one of the fixed timings above.
// Anything unrecognised -- blind (0), PAL 576 (17/18/21/22, no bootloader PAL
// support), or transient garbage -- maps to VIC 1 SD, the safest "something on
// screen" default (narrow/ambiguous SD signals resolve to progressive VIC 1
// rather than an interlaced or wide VIC here).
static const XboxVideoTiming *bl_resolve(uint8_t hw_vic, bool *widescreen, uint8_t *out_vic)
{
    switch (hw_vic) {
        case VIC_02_480p_60__4_3:
            *widescreen = false; *out_vic = VIC_02_480p_60__4_3; return &BL_TIMING_SD720;
        case VIC_03_480p_60_16_9:
        case VIC_07_480i_60_2x_Clk_16_9:
            *widescreen = true;  *out_vic = VIC_03_480p_60_16_9; return &BL_TIMING_SD720;
        case VIC_04_720p_60_16_9:
            *widescreen = true;  *out_vic = VIC_04_720p_60_16_9; return &BL_TIMING_720p;
        case VIC_05_1080i_60_16_9:
            *widescreen = true;  *out_vic = VIC_05_1080i_60_16_9; return &BL_TIMING_1080i;
        case VIC_01_VGA_640x480_4_3:
        case VIC_06_480i_60_2x_Clk_4_3:
        default:
            *widescreen = false; *out_vic = VIC_01_VGA_640x480_4_3; return &BL_TIMING_SD640;
    }
}

void bl_video_follow(void)
{
    static uint8_t  applied_vic = 0xFF; // VIC currently programmed (0xFF = none yet)
    static uint8_t  cand_vic    = 0xFF; // debounce candidate
    static uint32_t cand_since   = 0;

    uint8_t hw_vic = adv7511_read_register(0x3E) >> 2;

    // 30ms single stability debounce -- rides out the transient garbage 0x3E
    // reports while the GPU settles its own mode. A still-blind (0) reading is a
    // valid, stable candidate: it resolves to VIC 1 below.
    if (hw_vic != cand_vic) {
        cand_vic = hw_vic;
        cand_since = HAL_GetTick();
        return;
    }
    if ((HAL_GetTick() - cand_since) < 30) {
        return;
    }

    bool wide = false;
    uint8_t target_vic = VIC_00_VIC_Unavailable;
    const XboxVideoTiming *t = bl_resolve(hw_vic, &wide, &target_vic);

    if (target_vic == applied_vic) {
        return;
    }

    // Serialise with any in-flight TMDS cycle, as stand_alone_loop() does.
    if (adv7511_tmds_cycle_busy()) {
        return;
    }

    debug_log("[BL VIDEO] 0x3E VIC 0x%02X -> apply VIC 0x%02X (%ux%u %s)\r\n",
              hw_vic, target_vic, t->active_w, t->active_h, wide ? "16:9" : "4:3");

    // The tables carry one VIC per raster; the applied copy takes bl_resolve()'s, so the
    // 16:9 case sends VIC 3, not the table's 4:3 VIC 2.
    XboxVideoTiming applied_timing = *t;
    applied_timing.vic = target_vic;

    adv7511_tmds_cycle_begin();
    adv7511_apply_video_timing(&applied_timing, wide, false);
    adv7511_tmds_cycle_end();
    applied_vic = target_vic;
}
