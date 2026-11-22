// SPDX-License-Identifier: GPL-3.0-only
// The per-encoder mode tables, shared by both images (where they live: xbox_timing_rows.h):
// the application's xbox_get_video_timing() adds the per-profile calibration on top, the
// bootloader's recovery video (bl_video.c) uses them as they are.
#include "xbox_video_timing.h"
#include "xbox_timing_rows.h"
#include "adv7511_vic.h"
#include "user_config.h"
#include <string.h>

_Static_assert(sizeof(xbox_timing_tables_t) <= 512, "the tables outgrow bootloader.ld's 512-byte TIMING region");

#ifndef XHD_TIMING_TABLES_EXTERNAL
__attribute__((used, section(".timing_tables")))
const xbox_timing_tables_t g_timing_tables = {
    .magic = XBOX_TIMING_TABLES_MAGIC,
    .version = XBOX_TIMING_TABLES_VERSION,
    .size = sizeof(xbox_timing_tables_t),

    .mode_row = {
        0xFF,     ROW_0x01, ROW_0x02, ROW_0x03, ROW_0x04, ROW_0x05, ROW_0x06, ROW_0x08,   // 0x00-0x07
        ROW_0x08, ROW_0x09, ROW_0x0A, ROW_0x0A, ROW_0x0C, ROW_0x0C, ROW_0x0C, ROW_0x0F,   // 0x08-0x0F
        ROW_0x10, ROW_0x08, ROW_0x12, ROW_0x13, ROW_0x01, ROW_0x15,                       // 0x10-0x15
    },

    // Everything X-HD takes off the GPU is progressive (the encoder interlaces for its own
    // analog output), so there is no real "480i": 0x01/0x02 just run a different clock family
    // from 0x07/0x08. A 640-wide raster is VIC 1 (VIC 2 is 720-wide by definition; 640 16:9 is
    // VIC 3 here, the 0x13 synthetic stays VIC 1). The PAL 640x576 rows (0x05/0x12) have no CEA
    // VIC and use the 21/22 placeholder. 0x03/0x04/0x10 are the PAL-I 480-line hybrids (525
    // lines at 50 Hz): the line detector reads them inconsistently, so the arbiter treats them
    // as BIOS-authoritative (arb_is_bios_authoritative()).
    .shape = {
        [ROW_0x01] = { 640, 480, VIC_01_VGA_640x480_4_3, VIC_03_480p_60_16_9 },
        [ROW_0x02] = { 720, 480, VIC_02_480p_60__4_3, VIC_03_480p_60_16_9 },
        [ROW_0x03] = { 640, 480, VIC_01_VGA_640x480_4_3, VIC_03_480p_60_16_9 },
        [ROW_0x04] = { 720, 480, VIC_02_480p_60__4_3, VIC_03_480p_60_16_9 },
        [ROW_0x05] = { 640, 576, VIC_21_576i_50_2xClk_4_3, VIC_22_576i_50_2xClk_16_9 },
        [ROW_0x06] = { 720, 576, VIC_17_576p_50__4_3, VIC_18_576p_50_16_9 },
        [ROW_0x08] = { 720, 480, VIC_02_480p_60__4_3, VIC_03_480p_60_16_9 },
        [ROW_0x09] = { 960, 720, VIC_04_720p_60_16_9, VIC_04_720p_60_16_9 },
        [ROW_0x0A] = { 1280, 720, VIC_04_720p_60_16_9, VIC_04_720p_60_16_9 },
        [ROW_0x0C] = { 1920, 540, VIC_05_1080i_60_16_9, VIC_05_1080i_60_16_9 },
        [ROW_0x0F] = { 640, 480, VIC_01_VGA_640x480_4_3, VIC_03_480p_60_16_9 },
        [ROW_0x10] = { 640, 480, VIC_01_VGA_640x480_4_3, VIC_03_480p_60_16_9 },
        [ROW_0x12] = { 640, 576, VIC_21_576i_50_2xClk_4_3, VIC_22_576i_50_2xClk_16_9 },
        [ROW_0x13] = { 640, 480, VIC_01_VGA_640x480_4_3, VIC_01_VGA_640x480_4_3 },
        [ROW_0x15] = { 720, 480, VIC_02_480p_60__4_3, VIC_03_480p_60_16_9 },
    },

    // Per-(row, encoder) timing. Column order: XCALIBUR, CONEXANT, FOCUS.
    //
    // VS_Delay (delay_vs): where the GPU's picture starts, in lines from the leading edge of
    // vsync: VTOTAL - VSYNC_START - 1 (from the GPU's CRTC registers; 1080i per field, rounded down). It
    // matched every SD row tuned on screen; the rest had been copied from CEA (VIC 1 35, VIC 2/3
    // 36) or from another encoder and were 1-4 lines off. Encoders differ: Focus draws 720p/1080i
    // vsync 2 lines earlier than the others (27/22), Conexant's 0x01/0x02/0x0F 2 lines later (34).
    // Modes on one raster with different picture starts: Xcalibur 0x02 37 / 0x08 36, 0x07 36
    // on 0x01's row (37, xbox_base_timing()), Focus 0x07/0x08 38 / 0x11 36 (xbox_get_video_timing();
    // checked on screen). A switch between them moves only VS_Delay, applied in place
    // (adv7511_xbox.c raster_is_applied()). Standalone can't tell Focus 0x11 from 0x08: it sends
    // 38 for both. Too large a value runs DE past the field boundary ->
    // sink "unsupported" (on 1080i VS_Delay=24 failed, 6..23 are within range).
    //
    // HS_Delay (delay_hs): SD/PAL rows are tuned per mode (sync-adjust ON, pixel clock varies per
    // mode, regenerated sync is non-CEA). HD rows (sync-adjust OFF) follow CEA h_sync +
    // h_back_porch + the GPU's per-encoder hsync width -- more HS_Delay shifts the picture
    // LEFT:
    //   base = h_total - h_active - h_front_porch  (VIC 4 = 260, VIC 5 = 192)
    //   Each HD cell is where the GPU's picture starts: (HTOTAL+1) - HSYNC_START - 1 (from the
    //   GPU's CRTC registers). The Conexant GPU setup draws a wider hsync with the same end: START 1349 vs
    //   Xcalibur 1389 at 720p (300 vs 260), 1964 vs 2012 at 1080i (235 vs 187). The old 232 was
    //   centred on color bars by eye, 3 px left. Focus draws Conexant's hsyncs (same registers):
    //   300/235. 1440x1080i (0x0C) shares the row unmeasured on every encoder.
    // The Xcalibur column was cross-checked against the open-source NV2A CRTC
    // (xbox-linux fbdev.c/xcalibur.c): HS_Delay = (htotal+1) - hsyncstart gives SD 97 (we run 96)
    // and HD 261/193 for bare-CEA sync widths (we run 260/187). The extra +40 Linux/Cromwell adds
    // is that driver's non-CEA hsync_len=80 vs CEA 40 -- a sync-width difference, not a delay;
    // Conexant's and Focus's +40/+48 is the same kind of difference.
    //
    // Every cell is a pure, uncalibrated base value. Per-(encoder,profile) fine-tuning belongs
    // exclusively in user_config's enc_cal[].profile[].{hs,vs}_delay_offset, added on top once,
    // by xbox_get_video_timing() -- see its s_mode_index_to_profile. Baking a measured correction into a cell here
    // double-applies it for anyone whose enc_cal already holds a nonzero offset for that
    // (encoder, profile) slot (check with the `cal` CLI command before changing a row that isn't
    // provably at 0/0). Bases measured into the table so a fresh board (or one whose page 63 a
    // reflash wiped) is right at once: 0x0F's -- it runs a faster clock than 0x01 (26.87 vs
    // 24.42 MHz on Conexant: 165 = 122 + 43); on Xcalibur (~27.000 MHz) the GPU draws a 640 picture
    // at 0-639 of an 858 line, HSYNC_START 721, so it starts at 858 - 721 - 1 = 136 (from the GPU's
    // registers, checked on screen: 144 cropped 8 pixels on the left).
    // 0x13/0x15 (standalone-only, interlaced PAL 640/720) carry one set of values, Focus's, on
    // every encoder: taken from real Focus PAL packets (0x03 for 640-wide, 0x04 for 720-wide),
    // close to those rows but delay_vs differs by 2. 0x09 (960x720p, diagnostic only, no retail
    // title) is 301 on all three.
    .rows = {
        // Each cell: { delay_hs, delay_vs, hsync_placement, hsync_duration, vsync_placement, vsync_duration }
        //              XCALIBUR                    CONEXANT                    FOCUS
        [ROW_0x01] = { {96,  37, 16,  96, 10, 2 }, {122, 34, 15, 32, 10, 3 }, {180, 26, 75, 64, 18, 2 } },
        [ROW_0x02] = { {96,  37, 16,  62, 9,  6 }, {135, 34, 15, 32, 10, 3 }, {140, 26, 75, 64, 18, 2 } },
        [ROW_0x03] = { {96,  38, 55,  32, 8,  3 }, {255, 36, 55, 32, 8,  3 }, {144, 24, 79, 64, 20, 2 } },
        [ROW_0x04] = { {138, 38, 59,  32, 8,  3 }, {271, 36, 59, 32, 8,  3 }, {104, 24, 59, 64, 20, 2 } },
        [ROW_0x05] = { {143, 42, 127, 47, 6,  6 }, {133, 39, 15, 32, 9,  3 }, {144, 26, 79, 64, 22, 2 } },
        [ROW_0x06] = { {138, 42, 5,   45, 6,  6 }, {149, 39, 17, 33, 9,  3 }, {104, 26, 59, 64, 22, 2 } },
        [ROW_0x08] = { {96,  36, 16,  62, 9,  6 }, {120, 36, 17, 63, 8,  6 }, {120, 38, 17, 63, 8,  6 } },
        [ROW_0x09] = { {301, 25, 0,   0,  0,  0 }, {301, 25, 0,  0,  0,  0 }, {301, 25, 0,  0,  0,  0 } },
        [ROW_0x0A] = { {260, 25, 0,   0,  0,  0 }, {300, 25, 0,  0,  0,  0 }, {300, 27, 0,  0,  0,  0 } },
        [ROW_0x0C] = { {187, 20, 0,   0,  0,  0 }, {235, 20, 0,  0,  0,  0 }, {235, 22, 0,  0,  0,  0 } },
        [ROW_0x0F] = { {136, 37, 16,  96, 10, 2 }, {165, 34, 15, 32, 10, 3 }, {180, 26, 75, 64, 18, 2 } },
        [ROW_0x10] = { {178, 38, 81,  42, 1,  10}, {319, 34, 91, 32, 10, 3 }, {144, 24, 79, 64, 20, 2 } },
        [ROW_0x12] = { {143, 42, 87,  7,  6,  6 }, {180, 39, 48, 32, 9,  3 }, {144, 26, 79, 64, 22, 2 } },
        [ROW_0x13] = { {144, 26, 79,  64, 20, 2 }, {144, 26, 79, 64, 20, 2 }, {144, 26, 79, 64, 20, 2 } },
        [ROW_0x15] = { {104, 26, 59,  64, 20, 2 }, {104, 26, 59, 64, 20, 2 }, {104, 26, 59, 64, 20, 2 } },
    },
};
#endif

bool xbox_timing_tables_ok(const xbox_timing_tables_t *t) {
    return t->magic == XBOX_TIMING_TABLES_MAGIC && t->version == XBOX_TIMING_TABLES_VERSION &&
           t->size == sizeof(xbox_timing_tables_t);
}

#ifdef XHD_TIMING_TABLES_EXTERNAL
// Checked once (xbox_timing_tables_init()); after that a fixed address, which the compiler
// can index directly.
static bool s_tables_ok;
#define TABLES ((const xbox_timing_tables_t *)XBOX_TIMING_TABLES_ADDR)
bool xbox_timing_tables_init(void) {
    s_tables_ok = xbox_timing_tables_ok(TABLES);
    return s_tables_ok;
}
const xbox_timing_tables_t *xbox_timing_tables(void) {
    return s_tables_ok ? TABLES : NULL;
}
#else
bool xbox_timing_tables_init(void) {
    return xbox_timing_tables_ok(&g_timing_tables);
}
const xbox_timing_tables_t *xbox_timing_tables(void) {
    return &g_timing_tables;
}
#endif

bool xbox_timing_tables_missing(void) {
    return xbox_timing_tables() == NULL;
}

uint16_t xbox_pal_720x480_hs_delay(xbox_encoder encoder) {
    const xbox_timing_tables_t *T = xbox_timing_tables();
    return T ? T->rows[ROW_0x04][user_config_get_encoder_index(encoder)].delay_hs : 0;
}

bool xbox_base_timing(xbox_encoder encoder, uint8_t mode_index, bool widescreen, XboxVideoTiming *timing) {
    // 0x13-0x15 are standalone-only synthetic indices (real ones are 1-18), allowed here so
    // the application's calibration covers them too.
    const xbox_timing_tables_t *T = xbox_timing_tables();
    if (T == NULL || timing == NULL || mode_index >= sizeof T->mode_row || T->mode_row[mode_index] == 0xFF) {
        return false;
    }

    // 0x07 is 640-wide on Xcalibur: 0x01's row and family (see
    // xcal_mode_index_classify_instant()).
    const bool xcal_07 = (encoder == ENCODER_XCALIBUR && mode_index == 0x07);
    const uint8_t row = xcal_07 ? ROW_0x01 : T->mode_row[mode_index];
    const int enc = user_config_get_encoder_index(encoder);
    const EncTiming *et = &T->rows[row][enc];
    const RowShape *rs = &T->shape[row];

    // Clear the whole struct, padding included, so adv7511_timing_would_change() can compare
    // two timings with memcmp.
    memset(timing, 0, sizeof(*timing));
    timing->active_w = (mode_index == 0x0C) ? 1440 : rs->active_w;
    timing->active_h = rs->active_h;
    timing->delay_hs = et->delay_hs;
    timing->delay_vs = et->delay_vs - xcal_07;   // its picture starts a line above 0x01's
    timing->hsync_placement = et->hsync_placement;
    timing->hsync_duration = et->hsync_duration;
    timing->vsync_placement = et->vsync_placement;
    timing->vsync_duration = et->vsync_duration;
    timing->vic = widescreen ? rs->vic_16_9 : rs->vic_4_3;
    timing->sync_adjust_enabled = rs->vic_4_3 != VIC_04_720p_60_16_9 && rs->vic_4_3 != VIC_05_1080i_60_16_9;
    timing->interlaced_offset = rs->vic_4_3 == VIC_05_1080i_60_16_9;
    return true;
}
