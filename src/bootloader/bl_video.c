// SPDX-License-Identifier: GPL-3.0-only
#include "bl_video.h"

#include "../shared/adv7511.h" // pulls stm32.h (HAL_GetTick)
#include "../shared/adv7511_xbox.h"
#include "../shared/adv7511_vic.h"
#include "../shared/xbox_video_timing.h"
#include "../shared/debug.h"

extern xbox_encoder xb_encoder;   // main.c: the latched encoder from page 63, or UNKNOWN

// Each encoder's SD clocks (kHz) and the mode the recovery screen runs at each, from the
// sweep DB (run_bl_video_fixtures.c checks every recorded mode against these). The clock is
// what tells SD modes apart: 0x3E reads 0-3 for the same mode from one snapshot to the next.
// Modes that share a clock get one of them (the fixture lists which and why). mode_576 is
// the mode for a 0x3E 576 reading (17/18) at the same clock, 0 if the clock has no 576 mode.
// An unlatched encoder (ENCODER_UNKNOWN) uses the Xcalibur list.
typedef struct { uint16_t khz; uint8_t mode, mode_576; } BlClock;
static const BlClock k_xcal_clocks[] = {
    { 20800, 0x03, 0 },      // PAL 640x480 @ 50
    { 23400, 0x10, 0 },      // PAL 480-line (0x04 too)
    { 24545, 0x01, 0 },      // NTSC 640x480 (0x07 too)
    { 27000, 0x08, 0x06 },   // NTSC 720x480 (0x02/0x0F/0x11); 576 (0x05/0x12 too)
};
static const BlClock k_cx_clocks[] = {
    { 24420, 0x01, 0 },
    { 24625, 0x05, 0 },
    { 24937, 0x03, 0 },
    { 26874, 0x0F, 0 },
    { 27000, 0x08, 0 },      // 0x07/0x11 too
    { 27125, 0x12, 0 },
    { 27377, 0x02, 0 },
    { 27562, 0x10, 0 },      // 0x04 too
    { 27687, 0x06, 0 },
};
static const BlClock k_focus_clocks[] = {
    { 22680, 0x03, 0 },      // every PAL 480-line mode
    { 27000, 0x08, 0x06 },   // NTSC 480p; 576 (0x05/0x12 too)
    { 29454, 0x01, 0 },      // every NTSC interlaced mode
};

// HD runs ~74.2 MHz on every encoder; no SD mode is above 30 MHz.
#define BL_HD_PCLK_MIN_HZ 50000000UL

uint8_t bl_video_pick_mode(xbox_encoder enc, uint32_t pclk_hz, uint8_t vic3e)
{
    if (pclk_hz >= BL_HD_PCLK_MIN_HZ) {
        return vic3e == VIC_05_1080i_60_16_9 ? 0x0D : 0x0A;
    }
    const BlClock *c = k_xcal_clocks;
    uint8_t n = sizeof k_xcal_clocks / sizeof k_xcal_clocks[0];
    if (enc == ENCODER_CONEXANT) {
        c = k_cx_clocks;
        n = sizeof k_cx_clocks / sizeof k_cx_clocks[0];
    } else if (enc == ENCODER_FOCUS) {
        c = k_focus_clocks;
        n = sizeof k_focus_clocks / sizeof k_focus_clocks[0];
    }
    // The nearest clock: the real ones sit within +/-2 kHz, and a clock no mode runs still
    // gets the closest mode's sync.
    const BlClock *best = c;
    uint32_t best_err = 0xFFFFFFFFu;
    for (; n--; c++) {
        const uint32_t hz = c->khz * 1000UL;
        const uint32_t err = pclk_hz > hz ? pclk_hz - hz : hz - pclk_hz;
        if (err < best_err) {
            best_err = err;
            best = c;
        }
    }
    const bool is_576 = vic3e == VIC_17_576p_50__4_3 || vic3e == VIC_18_576p_50_16_9;
    return (is_576 && best->mode_576) ? best->mode_576 : best->mode;
}

void bl_video_follow(void)
{
    static uint8_t  applied = 0;   // mode_index on screen (0 = none yet)
    static uint8_t  cand    = 0;   // debounce candidate
    static uint32_t cand_since;

    const uint8_t vic3e = adv7511_read_register(0x3E) >> 2;
    const uint32_t pclk = adv7511_get_pixel_clock_hz();

    // A 0 clock is no reading: the ACR halts for ~600 ms after any signal change, our own
    // applies included, so it keeps what is on screen. Blind from the start (no clock at
    // all), 640x480 VIC 1 gives the TV a signal to lock on.
    uint8_t mode;
    if (pclk != 0) {
        mode = bl_video_pick_mode(xb_encoder, pclk, vic3e);
    } else if (applied == 0) {
        mode = 0x01;
    } else {
        return;
    }

    // Held for BL_VIDEO_SETTLE_MS: a mode set passes through transient clocks (Conexant's
    // ~27.5 MHz ones last up to ~150 ms -- adv7511.c's PCLK_STABLE_WINDOW_MS).
    if (mode != cand) {
        cand = mode;
        cand_since = HAL_GetTick();
        return;
    }
    if (mode == applied || (HAL_GetTick() - cand_since) < BL_VIDEO_SETTLE_MS) {
        return;
    }
    // Serialise with any in-flight TMDS cycle, as stand_alone_loop() does.
    if (adv7511_tmds_cycle_busy()) {
        return;
    }

    // SD goes out 4:3 (no EEPROM capture here, and 0x3E's VIC 2/3 flaps); HD is 16:9.
    XboxVideoTiming t;
    xbox_base_timing(xb_encoder, mode, false, &t);
    debug_log("[BL VIDEO] %lu kHz, 0x3E VIC %u -> mode 0x%02X (%ux%u VIC %u)\n",
              (unsigned long)(pclk / 1000u), vic3e, mode, t.active_w, t.active_h, t.vic);
    adv7511_tmds_cycle_begin();
    adv7511_apply_video_timing(&t, t.vic == VIC_04_720p_60_16_9 || t.vic == VIC_05_1080i_60_16_9, false);
    adv7511_tmds_cycle_end();
    applied = mode;
}
