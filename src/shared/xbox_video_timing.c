#include "xbox_video_timing.h"
#include "adv7511_vic.h"
#include "xbox_video_standalone.h"
#include "user_config.h"
#include "xbox_eeprom_video.h" // pure decode, no HAL
#include "vic_arbiter.h" // clock bands only (ARB_PAL50_SD_PCLK_LOW_HZ, the Conexant 640x576 PAR band) -- no HAL
#include <stddef.h>
#include <string.h>

// The EEPROM fields, the live encoder aspect and the Xcalibur mode index (this file is
// application-only, like smbus_probe.c).
#include "../application/smbus_probe.h"

// Must use the same precedence as the arbiter's arb_resolve_aspect(), or the decision and the
// apply disagree whenever the encoder aspect (WSS) and the EEPROM differ.
bool sd_resolve_widescreen(xbox_encoder encoder, bool signal_widescreen) {
    switch (user_config_get()->aspect_ratio) {
        // The user's setting wins; WSS/EEPROM only in Auto (arb_resolve_aspect()'s order).
        case ASPECT_RATIO_16_9: return true;
        case ASPECT_RATIO_4_3:  return false;
        default: {
            // The live encoder aspect beats the EEPROM bit (it is rewritten per mode/title),
            // for all three encoders. Skipping Xcalibur here put the EEPROM's 4:3 on the
            // BIOS -> standalone handoff, corrected by the arbiter 0.8 s later.
            bool wss_wide;
            if (xbox_encoder_get_wss_widescreen(encoder, &wss_wide)) {
                return wss_wide;
            }
            // Then the dashboard's Widescreen setting (the EEPROM bit, all titles, captured
            // at console boot), else the caller's guess.
            uint32_t vset;
            if (xbox_eeprom_get_video_settings(&vset)) {
                return xbox_eeprom_decode_widescreen(vset);
            }
            return signal_widescreen;
        }
    }
}

// mode_index (1-18, plus the standalone synthetics 0x13-0x15) -> timing profile, the unit of
// per-encoder hs/vs calibration (user_config's enc_cal[].profile[]). mode_indices that share a
// timing row share a profile; 0xFF = not a mode. Keep in sync with XHD_NUM_TIMING_PROFILES
// (the static assert checks the length).
// 0x01 (640-wide) and 0x02 (720-wide) get separate profiles, since the clock can't tell them
// apart reliably. 0x07 shares 0x08/0x11's profile 1 (on Xcalibur it uses 13 -- see
// xbox_get_video_timing()). Profiles 12 and 15 are unused (old slots, kept rather than
// renumbering).
static const uint8_t s_mode_index_to_profile[22] = {
    /* 0x00 */ 0xFF, // unused, mode_index is 1-18
    /* 0x01 */ 13,   // NTSC 640-wide VGA/SDTV (~24.4MHz) -- own profile, split from 0x02
    /* 0x02 */ 0,    // NTSC 720-wide SDTV -- own profile, split from 0x01
    /* 0x03 */ 2,    // PAL row 0 (case 0x03/0x04/0x05/0x06/0x10/0x12)
    /* 0x04 */ 3,    // PAL row 1
    /* 0x05 */ 4,    // PAL row 2
    /* 0x06 */ 5,    // PAL row 3
    /* 0x07 */ 1,    // NTSC SD 480p, 720-wide -- shares 0x08/0x11's profile
    /* 0x08 */ 1,    // NTSC SD 480p, 720-wide (case 0x07/0x08/0x11; shares profile w/ 0x07/0x11)
    /* 0x09 */ 8,    // 960x720 HD, its own case -- NOT part of the 480p group above
    /* 0x0A */ 9,    // 1280x720p (case 0x0A/0x0B)
    /* 0x0B */ 9,
    /* 0x0C */ 10,   // 1440x1080i, its own case
    /* 0x0D */ 11,   // 1920x1080i (case 0x0D/0x0E)
    /* 0x0E */ 11,
    /* 0x0F */ 17,   // NTSC 640-wide "1.1:1 PAR" -- own profile (base row shared with 0x01,
                      // see the 0x0F case-block comment)
    /* 0x10 */ 6,    // PAL row 4
    /* 0x11 */ 1,    // NTSC SD 480p, 720-wide (shares profile with 0x07/0x08)
    /* 0x12 */ 7,    // PAL row 5
    /* 0x13 */ 14,   // Standalone-only synthetic: interlaced/SDTV-family, PAL-region/50Hz --
                      // never a real mode_index, only produced by
                      // xbox_get_standalone_timing(). Genuinely 640-wide; 0x15 is its
                      // dedicated 720-wide sibling.
    /* 0x14 */ 17,   // Folded into 0x0F's own row in xbox_get_video_timing() -- shares 0x0F's
                      // profile so any future retuning of 0x0F (which is live-testable via
                      // real BIOS packets, unlike 0x14) keeps this fallback's calibration
                      // current. Profile 15 (its former slot) is unused (left in the array
                      // rather than renumbering 16/17 down).
    /* 0x15 */ 16,   // Standalone-only synthetic: interlaced/SDTV-family 720-wide,
                      // PAL-region/50Hz -- the 720-wide sibling of the 0x13/0x14 split;
                      // 0x02 alone still covers the real NTSC-region case.
};
_Static_assert(sizeof(s_mode_index_to_profile) == 22, "mode_index_to_profile table must cover mode_index 0-18 plus the synthetic 0x13/0x14/0x15");

static uint8_t s_last_profile_index = 0xFF;

uint8_t xbox_video_timing_last_profile_index(void) {
    return s_last_profile_index;
}

bool xbox_profile_is_50hz(uint8_t profile) {
    // The PAL-I rows (mode_index 0x03-0x06/0x10/0x12) and the standalone PAL synthetics
    // (0x13/0x15). The console runs every one of those as a 576-line SDTV output, so the
    // encoder -- and with it the GPU raster -- runs at 50Hz whatever the active height.
    // PAL-60 (0x08xx19/1A/1B) shares the NTSC rows.
    return (profile >= 2u && profile <= 7u) || profile == 14u || profile == 16u;
}


// One encoder's timing for one mode row (the NTSC/HD and PAL tables below).
typedef struct { uint16_t delay_hs; uint8_t delay_vs, hsync_placement, hsync_duration, vsync_placement, vsync_duration; } EncTiming;

// PAL modes: per-(mode, encoder) timing for the 6 PAL mode indices. Row order: 0x03, 0x04,
// 0x05, 0x06, 0x10, 0x12. Column order: XCALIBUR, CONEXANT, FOCUS.
static const EncTiming pal_table[6][3] = {
    { {96,  36, 55,  32, 8,  3 }, {255, 36, 55, 32, 8,  3 }, {144, 24, 79, 64, 20, 2 } },
    { {138, 36, 59,  32, 8,  3 }, {271, 36, 59, 32, 8,  3 }, {104, 24, 59, 64, 20, 2 } },
    { {143, 41, 127, 47, 6,  6 }, {133, 39, 15, 32, 9,  3 }, {144, 26, 79, 64, 22, 2 } },
    { {138, 42, 5,   45, 6,  6 }, {149, 39, 17, 33, 9,  3 }, {104, 26, 59, 64, 22, 2 } },
    { {178, 34, 81,  42, 1,  10}, {319, 34, 91, 32, 10, 3 }, {144, 24, 79, 64, 20, 2 } },
    { {143, 41, 87,  7,  6,  6 }, {180, 39, 48, 32, 9,  3 }, {144, 26, 79, 64, 22, 2 } },
};

uint16_t xbox_pal_720x480_hs_delay(xbox_encoder encoder) {
    return pal_table[1][user_config_get_encoder_index(encoder)].delay_hs;   // row 1 = 0x04
}

bool xbox_get_video_timing(xbox_encoder encoder, uint8_t mode_index, bool widescreen, XboxVideoTiming *timing) {
    // 0x13-0x15 are standalone-only synthetic indices (real ones are 1-18), allowed here so
    // they share the calibration below.
    if (timing == NULL || mode_index < 1 || mode_index > 0x15) return false;


    // Clear the whole struct, padding included, so adv7511_timing_would_change() can compare
    // two timings with memcmp. Every field below is set by a branch or deliberately 0.
    memset(timing, 0, sizeof(*timing));
    timing->sync_adjust_enabled = true;

    switch (mode_index) {
        case 0x09: {
            // 960x720p (diagnostic only, no retail title). delay_hs 301 on all three encoders.
            timing->active_w = 960;
            timing->active_h = 720;
            timing->delay_hs = 301;
            timing->delay_vs = 25;
            timing->sync_adjust_enabled = false;
            timing->vic = VIC_04_720p_60_16_9;
            break;
        }
        // NTSC SD (0x01/0x02, 0x0F), NTSC 480p (0x07/0x08/0x11) and HD (0x0A-0x0E); 0x09 is
        // above, PAL below. Everything X-HD takes off the GPU is progressive (the encoder
        // interlaces for its own analog output), so there is no real "480i" here -- 0x01/0x02
        // just run a different clock family from 0x07/0x08.
        // 0x07 uses 0x08's 720-wide row (Xcalibur: 640-wide, see below). 0x0A/0x0B and
        // 0x0C-0x0E each share one row; 0x0C differs only in width (1440), set below.
        // 0x0F, NTSC's 640-wide "1.1:1 PAR" mode, uses 0x01's row with its own profile (17).
        // Its aspect comes from the normal BIOS path; nothing here forces it.
        case 0x01: case 0x02: case 0x07: case 0x08:
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x14: {
            typedef struct { uint16_t active_w, active_h; } ModeGeometry;

            // idx_map[mode_index] -> row (0-4) into the tables below; 0xFF = handled elsewhere
            // (PAL, 0x09, 0x13) or invalid. 0x07/0x11 share 0x08's row; 0x0F shares 0x01's.
            // 0x14 (standalone, interlaced NTSC 640) also uses row 0: its geometry matches, and
            // its caller always passes widescreen = false for 640 (anamorphic travels
            // separately), so it resolves VIC 1 the same way.
            static const uint8_t idx_map[21] = {
                0xFF,       // 0x00
                0, 1,       // 0x01, 0x02
                0xFF, 0xFF, 0xFF, 0xFF, // 0x03-0x06 (PAL, own case block)
                2, 2,       // 0x07, 0x08 (0x07 shares 0x08's row -- see block comment)
                0xFF,       // 0x09 (own dedicated case above)
                3, 3, 4, 4, 4, // 0x0A, 0x0B, 0x0C, 0x0D, 0x0E
                0,          // 0x0F (shares 0x01's row -- own profile still gives it
                            // independent hs/vs calibration, see s_mode_index_to_profile)
                0xFF,       // 0x10 (PAL)
                2,          // 0x11 (shares 0x08's row)
                0xFF,       // 0x12 (PAL)
                0xFF,       // 0x13 (own dedicated synthetic case below, still 640-wide but
                            // NOT folded here -- see that case block's own comment)
                0,          // 0x14 (folded into 0x0F/0x01's row)
            };
            static const ModeGeometry geom[5] = {
                {640, 480},  // 0x01 (+ 0x0F)
                {720, 480},  // 0x02
                {720, 480},  // 0x08 (+ 0x07, 0x11)
                {1280, 720}, // 0x0A (+ 0x0B)
                {1920, 540}, // 0x0C (+ 0x0D, 0x0E) -- 0x0C's own 1440-wide override below
            };
            // Row order matches idx_map above. Column order: XCALIBUR, CONEXANT, FOCUS.
            // VS_Delay column (struct field [1]) = exact CEA vsync + back-porch for the
            // row's VIC, encoder-independent, whole lines: VIC 1 (640x480p) 35 / VIC 2/3
            // (720x480p) 36 / VIC 4 (720p60) 25 / VIC 5 (1080i60) 20. Each value is also
            // the hard vertical ceiling (v_total_field - v_active_field - v_front_porch);
            // exceeding it runs DE past the field boundary -> sink "unsupported" (fatal on
            // interlaced VIC 5, where VS_Delay=24 fails and 6..23 are within range).
            //
            // HS_Delay column (field [0]): SD/PAL rows are tuned per mode (sync-adjust ON,
            // pixel clock varies per mode, regenerated sync is non-CEA). HD rows
            // (sync-adjust OFF) follow CEA h_sync + h_back_porch + a fixed per-encoder
            // pixel-pipeline offset -- more HS_Delay shifts the picture LEFT:
            //   base = h_total - h_active - h_front_porch  (VIC 4 = 260, VIC 5 = 192)
            //   Xcalibur +0   Conexant +40   Focus +40 (matches Conexant at 720p/1080i).
            //   CX VIC4 300 and CX VIC5 232 are centred on color bars -- the +40 is a real
            //   phase gap, not slop, so HD HS_Delay cannot be dropped to bare CEA.
            //
            // Xcalibur column cross-checked against the open-source NV2A CRTC
            // (xbox-linux fbdev.c/xcalibur.c): HS_Delay = (htotal+1) - hsyncstart gives SD 97
            // (we run 96) and HD 261/193 for bare-CEA sync widths (we run 260/187). The extra
            // +40 Linux/Cromwell adds is that driver's non-CEA hsync_len=80 vs CEA 40 -- a
            // sync-width difference, not a delay. Conexant/Focus's own +40 is an empirically
            // measured per-encoder pipeline offset.
            //
            // Every cell here must be a pure, uncalibrated base value. Per-(encoder,profile)
            // fine-tuning belongs exclusively in user_config's enc_cal[].profile[].{hs,vs}_
            // delay_offset, added on top of whatever is here once, unconditionally, below --
            // see s_mode_index_to_profile / the bound-check block after this switch. Baking
            // a measured correction into a literal here double-applies it for anyone whose
            // enc_cal already holds a nonzero offset for that (encoder, profile) slot (check
            // with the `cal` CLI command before changing a row that isn't provably at 0/0).
            static const EncTiming t[5][3] = {
                { {96,  35, 16, 96, 10, 2}, {122, 35, 15, 32, 10, 3}, {180, 26, 75, 64, 18, 2} }, // 0x01 (+ 0x0F)
                { {96,  36, 16, 62, 9,  6}, {135, 36, 15, 32, 10, 3}, {140, 26, 75, 64, 18, 2} }, // 0x02
                { {96,  36, 16, 62, 9,  6}, {120, 36, 17, 63, 8,  6}, {120, 36, 17, 63, 8,  6} }, // 0x08 (+ 0x07, 0x11)
                { {260, 25, 0,  0,  0,  0}, {300, 25, 0,  0,  0,  0}, {300, 25, 0,  0,  0,  0} }, // 0x0A (+ 0x0B)
                { {187, 20, 0,  0,  0,  0}, {232, 20, 0,  0,  0,  0}, {232, 20, 0,  0,  0,  0} }, // 0x0C (+ 0x0D, 0x0E)
            };

            // 0x07 is 640-wide on Xcalibur (row 0, 0x01's family), 720-wide elsewhere (see
            // xcal_mode_index_classify_instant()).
            uint8_t row = (encoder == ENCODER_XCALIBUR && mode_index == 0x07) ? 0 : idx_map[mode_index];
            uint8_t enc = (uint8_t)user_config_get_encoder_index(encoder);
            const EncTiming *mt = &t[row][enc];

            timing->active_w = geom[row].active_w;
            timing->active_h = geom[row].active_h;
            if (mode_index == 0x0C) timing->active_w = 1440; // 0x0C alone is 1440-wide; 0x0D/0x0E share this row at 1920
            timing->delay_hs = mt->delay_hs;
            timing->delay_vs = mt->delay_vs;
            // 0x0F (NTSC 640x480 1.1:1 PAR) runs a faster clock than 0x01 (26.87 vs
            // 24.42 MHz on Conexant), so it needs its own base: 165 on Conexant (122 + 43). A
            // board still carrying a nonzero profile-17 hs offset from before this base gets
            // 165 + offset; zero it (`hs 165` with 0x0F on screen).
            if (mode_index == 0x0F && encoder == ENCODER_CONEXANT) {
                timing->delay_hs = 165;
            }
            // Xcalibur 0x0F (~27.000 MHz): delay_hs 144 (measured live, centring the raster),
            // delay_vs 35 (row 0's CEA 480-line value). Baked in so a fresh board, or one
            // whose page 63 was wiped by a reflash, is right at once.
            if (mode_index == 0x0F && encoder == ENCODER_XCALIBUR) {
                timing->delay_hs = 144;
                timing->delay_vs = 35;
            }
            timing->hsync_placement = mt->hsync_placement;
            timing->hsync_duration = mt->hsync_duration;
            timing->vsync_placement = mt->vsync_placement;
            timing->vsync_duration = mt->vsync_duration;

            if (mode_index == 0x0A || mode_index == 0x0B) {
                timing->sync_adjust_enabled = false;
                timing->vic = VIC_04_720p_60_16_9;
            } else if (mode_index == 0x0C || mode_index == 0x0D || mode_index == 0x0E) {
                timing->interlaced_offset = 1;
                timing->sync_adjust_enabled = false;
                timing->vic = VIC_05_1080i_60_16_9;
            } else {
                // NTSC SD/480p: a 640-wide raster is VIC 1 (VIC 2 is 720-wide by definition).
                timing->vic = (!widescreen && timing->active_w == 640) ?
                              VIC_01_VGA_640x480_4_3 :
                              (widescreen ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3);
            }
            break;
        }

        // Standalone-only synthetic indices for interlaced (SDTV-family) PAL content:
        // 0x13 640-wide, 0x15 720-wide (0x14, the NTSC 640 one, uses row 0 above). Kept
        // apart from 0x10/0x04: close to their Focus values but delay_vs differs by 2.
        case 0x13: case 0x15: {
            // One set of values, Focus's, not per encoder like the other tables: these rows
            // were made for Focus's interlaced PAL content. Taken from real Focus PAL packets
            // (0x03 for 640-wide, 0x04 for 720-wide): hsync_placement 79/59, vsync_placement
            // 20, delay_hs 144/104.
            bool is_720 = (mode_index == 0x15);
            timing->delay_hs = is_720 ? 104 : 144;
            timing->delay_vs = 26;
            timing->hsync_placement = is_720 ? 59 : 79;
            timing->hsync_duration = 64;
            timing->vsync_placement = 20;
            timing->vsync_duration = 2;
            // 0x13 is 640-wide, 0x15 720-wide (s_mode_lut's layout).
            timing->active_w = is_720 ? 720 : 640;
            timing->active_h = 480;
            timing->vic = is_720 ? (widescreen ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3)
                                 : VIC_01_VGA_640x480_4_3;
            break;
        }

        // --- PAL Modes ---
        // Timing from pal_table (above).
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x10: case 0x12: {
            static const uint16_t pal_wh[6][2] = { {640,480}, {720,480}, {640,576}, {720,576}, {640,480}, {640,576} };

            uint8_t row = (mode_index == 0x03) ? 0 : (mode_index == 0x04) ? 1 : (mode_index == 0x05) ? 2 :
                          (mode_index == 0x06) ? 3 : (mode_index == 0x10) ? 4 : 5;
            uint8_t enc = (uint8_t)user_config_get_encoder_index(encoder);
            const EncTiming *t = &pal_table[row][enc];

            timing->active_w = pal_wh[row][0];
            timing->active_h = pal_wh[row][1];
            timing->delay_hs = t->delay_hs;
            timing->delay_vs = t->delay_vs;
            timing->hsync_placement = t->hsync_placement;
            timing->hsync_duration = t->hsync_duration;
            timing->vsync_placement = t->vsync_placement;
            timing->vsync_duration = t->vsync_duration;
            // VIC by geometry: 0x06 (720x576) VIC 17/18; 0x03/0x10 (640x480) VIC 1;
            // 0x04 (720x480) VIC 2/3; 0x05/0x12 (640x576, no CEA VIC) the 21/22 placeholder.
            timing->vic = (row == 3) ? (widescreen ? VIC_18_576p_50_16_9 : VIC_17_576p_50__4_3)
                        : (row == 0 || row == 4) ? (widescreen ? VIC_03_480p_60_16_9 : VIC_01_VGA_640x480_4_3)
                        : (row == 1) ? (widescreen ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3)
                        : (widescreen ? VIC_22_576i_50_2xClk_16_9 : VIC_21_576i_50_2xClk_4_3);
            // 0x03/0x04/0x10 are the PAL-I 480-line hybrids (525 lines at 50 Hz); the line
            // detector reads them inconsistently (VIC 1/2 or the 576 family), so the arbiter
            // treats them as BIOS-authoritative (arb_is_bios_authoritative()).
            break;
        }

        default:
            return false;
    }

    // The per-(profile, encoder) hs/vs calibration offset (user_config's profile_cal_t),
    // applied once. The profile is recorded only on success. Xcalibur 0x07 uses profile 13
    // (0x01's family -- 640-wide there), not the table's 1.
    s_last_profile_index = (encoder == ENCODER_XCALIBUR && mode_index == 0x07) ? 13 : s_mode_index_to_profile[mode_index];

    // Bound-check: the table is hand-maintained, and an out-of-range profile would read into
    // the next encoder's slot. Out of range = no calibration.
    if (s_last_profile_index != 0xFF && s_last_profile_index < XHD_NUM_TIMING_PROFILES) {
        int enc_idx = user_config_get_encoder_index(encoder);
        const profile_cal_t *pc = &user_config_get()->enc_cal[enc_idx].profile[s_last_profile_index];

        if ((int32_t)timing->delay_hs + pc->hs_delay_offset >= 0) {
            timing->delay_hs = (uint16_t)(timing->delay_hs + pc->hs_delay_offset);
        }
        if ((int32_t)timing->delay_vs + pc->vs_delay_offset >= 0) {
            timing->delay_vs = (uint16_t)(timing->delay_vs + pc->vs_delay_offset);
        }
    }

    return true;
}

// xbox_get_standalone_timing()'s NTSC-vs-PAL clock bands (the reasoning is at each use).
#define INTERLACED_NTSC_PCLK_MIN_HZ             26000000UL   // any encoder; Focus-measured
#define FOCUS_STANDALONE_NTSC480_PCLK_MIN_HZ    25000000UL
#define CX_STANDALONE_NTSC480_LOW_PCLK_MAX_HZ   24700000UL
#define CX_STANDALONE_NTSC480_PAR_PCLK_LOW_HZ   26700000UL
#define CX_STANDALONE_NTSC480_PAR_PCLK_HIGH_HZ  27450000UL
#define XCAL_STANDALONE_NTSC480_LOW_PCLK_MIN_HZ 24400000UL
#define CX_RETAIL_NTSC480I_PCLK_LOW_HZ          28500000UL

bool xbox_get_standalone_timing(xbox_encoder encoder, uint8_t standalone_mode, bool widescreen, bool interlaced, uint32_t pclk_hz, XboxVideoTiming *timing) {
    // pal_i_50: is this a PAL-I 50 Hz console? Then a 480-line SD raster is a PAL-table row
    // (640 -> 0x03, 720 -> 0x04; mode 0x03 = 640x480@50, 0x04 = 720x480@50)
    // rather than NTSC's (0x01/0x08) -- shapes too close to tell apart from the signal, with
    // different sync timing. The starting point is the EEPROM VideoStandard (captured at
    // console boot, no BIOS support needed); a PAL-I console set to 60 Hz counts as NTSC.
    // NTSC when there is no capture. The clock overrides below correct it per encoder.
    uint32_t vstd, vset;
    bool have_std = xbox_eeprom_get_video_standard(&vstd);
    bool have_set = xbox_eeprom_get_video_settings(&vset);
    bool pal_i_50 = have_std &&
        xbox_eeprom_decode_video_standard(vstd) == XBOX_EEPROM_VIDEO_STD_PAL_I &&
        xbox_eeprom_resolve_refresh(vstd, have_set ? vset : 0u) != XBOX_EEPROM_REFRESH_60HZ;

    // Interlaced content at >= 26 MHz is NTSC whatever the EEPROM says: the PAL-region
    // interlaced clock is ~22.68 MHz, the NTSC one ~29.455 MHz (Focus-measured). Not gated on
    // the encoder -- `interlaced` comes from Focus's family check or Conexant's 480i check.
    // Uses the caller's clock, not a fresh read: this runs inside the apply, where CTS
    // reads 0.
    pal_i_50 = pal_i_50 && !(interlaced && pclk_hz >= INTERLACED_NTSC_PCLK_MIN_HZ);

    // Focus, any scan: every Focus NTSC SD clock is >= 25 MHz (640p ~25.175, 720p ~27.000,
    // interlaced ~29.455) and every Focus PAL SD clock ~22.68 MHz, so >= 25 MHz is NTSC. (On
    // Focus this covers the interlaced rule above too.)
    if (encoder == ENCODER_FOCUS && pclk_hz >= FOCUS_STANDALONE_NTSC480_PCLK_MIN_HZ) {
        pal_i_50 = false;
    }

    // Conexant/Xcalibur: a PAL-I console's 60 Hz (NTSC-shaped) content. With pal_i_50 left
    // true, NTSC content got PAL sync placement -- ~133 px off, and HDMI sinks drop the
    // picture entirely. Conexant's NTSC clocks: 640x480 ~24.42 MHz (PAL ~24.94; split at
    // 24.70), 640 PAR ~26.87 and 720x480 ~27.38 MHz (PAL ~27.56; band [26.70, 27.45]). The
    // narrow-576 clocks overlapping these bands never get here (576i_640 maps to 0x05
    // directly).
    // Xcalibur: its NTSC 640 clock is ~24.545 MHz, but its real PAL clocks (20.8 and
    // 23.4 MHz) are also under 24.70, so its low band starts at 24.40 MHz instead of 0
    // (check_xcal_standalone_parity() in run_standalone_fixtures.c covers both).
    const uint32_t low_pclk_min_hz = (encoder == ENCODER_XCALIBUR) ? XCAL_STANDALONE_NTSC480_LOW_PCLK_MIN_HZ : 0UL;
    if ((encoder == ENCODER_CONEXANT || encoder == ENCODER_XCALIBUR) && pclk_hz > 0 &&
        ((pclk_hz >= low_pclk_min_hz && pclk_hz < CX_STANDALONE_NTSC480_LOW_PCLK_MAX_HZ) ||
         (pclk_hz >= CX_STANDALONE_NTSC480_PAR_PCLK_LOW_HZ &&
          pclk_hz <= CX_STANDALONE_NTSC480_PAR_PCLK_HIGH_HZ))) {
        pal_i_50 = false;
    }

    // No EEPROM capture (a board-only reset: the console broadcasts it only at its own boot).
    // Focus: no NTSC SD clock is below 24 MHz (they run 25.175-29.455), so a clock under
    // ARB_PAL50_SD_PCLK_LOW_HZ is the ~22.68 MHz PAL hybrid.
    if (encoder == ENCODER_FOCUS && !have_std &&
        pclk_hz > 0 && pclk_hz < ARB_PAL50_SD_PCLK_LOW_HZ) {
        pal_i_50 = true;
    }

    // No EEPROM capture, Conexant: the gaps between its NTSC bands, [24.70, 26.70) and
    // (27.45, 28.50) MHz, are PAL by elimination. The upper gap stops below retail NTSC 480i
    // (~29.454 MHz); no PAL SD clock is above ~27.7 MHz.
    if (encoder == ENCODER_CONEXANT && !have_std && pclk_hz > 0) {
        if ((pclk_hz >= CX_STANDALONE_NTSC480_LOW_PCLK_MAX_HZ &&
             pclk_hz < CX_STANDALONE_NTSC480_PAR_PCLK_LOW_HZ) ||
            (pclk_hz > CX_STANDALONE_NTSC480_PAR_PCLK_HIGH_HZ &&
             pclk_hz < CX_RETAIL_NTSC480I_PCLK_LOW_HZ)) {
            pal_i_50 = true;
        }
    }

    // [width_is_720][interlaced][pal_i_50] -> mode_index.
    static const uint8_t s_mode_lut[2][2][2] = {
        { {0x01, 0x03}, {0x14, 0x13} }, // 640-wide
        { {0x08, 0x04}, {0x02, 0x15} }, // 720-wide
    };
    uint8_t mode_index;
    switch (standalone_mode) {
        case XBOX_VIDEO_480p_640:
            // A narrow raster: 0x01 (640x480), or 0x03 on PAL; interlaced -> 0x14/0x13. (A
            // real 480p-640 0x07 classifies wide on Conexant/Focus and goes to 0x08.)
            // Clock-specific modes first, so standalone gets the same timing as their BIOS
            // packets:
            //  - Conexant 26.70-27.45 MHz: 0x0F, NTSC 640 1.1:1 PAR (26.87 MHz, base 165);
            //  - Conexant 27.45-27.65 MHz: 0x10, PAL 640 1.1:1 PAR (~27.562 MHz);
            //  - Xcalibur: 0x10 at 22-24 MHz (~23.4 MHz, apart from 0x03's ~20.8) when no
            //    register width has been seen. Its 0x0F/0x10 are register-identical to the
            //    720-wide 0x02/0x04 and classify wide, so they never reach this case.
            if (encoder == ENCODER_CONEXANT && pclk_hz >= CX_STANDALONE_NTSC480_PAR_PCLK_LOW_HZ &&
                pclk_hz <= CX_STANDALONE_NTSC480_PAR_PCLK_HIGH_HZ) {
                mode_index = 0x0F;
            } else if (encoder == ENCODER_CONEXANT && pclk_hz > CX_STANDALONE_NTSC480_PAR_PCLK_HIGH_HZ &&
                       pclk_hz <= 27650000UL) {
                mode_index = 0x10;
            } else if (encoder == ENCODER_XCALIBUR) {
                if (pclk_hz > 22000000UL && pclk_hz < 24000000UL) {
                    mode_index = 0x10;
                } else {
                    mode_index = s_mode_lut[0][interlaced][pal_i_50];
                }
            } else {
                mode_index = s_mode_lut[0][interlaced][pal_i_50];
            }
            break;
        case XBOX_VIDEO_480p_720:
            // Interlaced PAL -> the synthetic 0x15 (0x02 stays the NTSC one).
            // Xcalibur: 0x02 and 0x08 have identical timing but different profiles, so its
            // register mode index (0x50 bit 15, SDTV) picks the right one -- otherwise a
            // calibration on either would be applied to the wrong mode.
            if (encoder == ENCODER_XCALIBUR) {
                if (xbox_xcal_mode_index() == 0x02) {
                    mode_index = 0x02;
                } else {
                    mode_index = s_mode_lut[1][interlaced][pal_i_50];
                }
            } else {
                mode_index = s_mode_lut[1][interlaced][pal_i_50];
            }
            break;
        case XBOX_VIDEO_720p:
            mode_index = 0x0A;
            break;
        case XBOX_VIDEO_1080i:
            mode_index = 0x0D;
            break;
        case XBOX_VIDEO_576p_720:
            mode_index = 0x06;
            break;
        case XBOX_VIDEO_576i_640:
            // 0x05 vs 0x12 (both 640x576): on Conexant by clock (24.625 vs 27.125 MHz, the
            // arbiter's narrow-576 bands).
            if (encoder == ENCODER_CONEXANT && arb_cx_clock_is_640x576_par(pclk_hz)) {
                mode_index = 0x12;
            } else if (encoder == ENCODER_XCALIBUR) {
                // Xcalibur runs both at 27.000 MHz: its register mode index decides, else 0x05.
                if (xbox_xcal_mode_index() == 0x12) {
                    mode_index = 0x12;
                } else {
                    mode_index = 0x05;
                }
            } else {
                mode_index = 0x05;
            }
            break;
        default:
            return false;
    }
    return xbox_get_video_timing(encoder, mode_index, widescreen, timing);
}

uint16_t xbox_refresh_mhz(uint16_t active_h, bool interlaced, uint32_t pclk_hz, bool fifty_hz) {
    if (pclk_hz == 0) {
        return 0;      // no clock
    }
    // PAL has no 1000/1001 variant, and the 480-line PAL-I rows (525 lines paced to a 625-line
    // 50Hz encoder frame by the console's line generator) have no whole-number line length to
    // recover it from anyway: 23.400MHz / (525 x 50) = 891.43.
    if (fifty_hz) {
        return 50000u;
    }
    // Lines per frame (1080i: per frame of two fields).
    const uint32_t vtot = interlaced ? 1125u : (active_h >= 720) ? 750u : 525u;
    // clock / lines, in mHz: frame rate x line length.
    const uint32_t num = pclk_hz / vtot * 1000u;
    uint32_t total = 0, best_err = 0xFFFFFFFFu;
    uint32_t f = 60000u;   // nominal rate; also tries 59.94
    for (;;) {
        const uint32_t frame = interlaced ? f / 2u : f;
        const uint32_t h = (num + frame / 2u) / frame;
        const int32_t err = (int32_t)(num - h * frame);
        const uint32_t abs_err = (uint32_t)(err < 0 ? -err : err);
        if (abs_err < best_err) {
            best_err = abs_err;
            total = h * vtot;
        }
        if (f != 60000u) {
            break;
        }
        f = 59940u;
    }
    if (interlaced) {
        total /= 2u;   // a field is half an interlaced frame
    }
    if (total == 0) {
        return 0;      // no clock
    }
    // clock / total in mHz, rounded, without 64-bit math.
    return (uint16_t)((pclk_hz / total) * 1000u + ((pclk_hz % total) * 1000u + total / 2u) / total);
}

bool xbox_position(xbox_encoder enc, bool is_hs, int16_t *offset, int16_t *base, bool set) {
    // xbox_get_video_timing() bound-checks the same index before using it: the profile map is
    // hand-maintained, so treat anything out of range like "no mode".
    if (s_last_profile_index >= XHD_NUM_TIMING_PROFILES) {
        return false;
    }
    // profile_cal_t is packed: copy the field bytewise, never through an int16_t* (HardFault).
    uint8_t *f = (uint8_t *)&user_config_get()->enc_cal[user_config_get_encoder_index(enc)]
                     .profile[s_last_profile_index]
                 + (is_hs ? offsetof(profile_cal_t, hs_delay_offset) : offsetof(profile_cal_t, vs_delay_offset));
    int16_t cur_off;
    memcpy(&cur_off, f, sizeof cur_off);
    if (set || base) {
        // The base is the live register minus the offset already applied.
        // HS = 0x35[7:0]<<2 | 0x36[7:6], stored -1; VS = 0x36[5:0].
        uint8_t r36 = adv7511_read_register(0x36);
        const int16_t b = (int16_t)((is_hs ? (((uint16_t)adv7511_read_register(0x35) << 2) | (r36 >> 6)) + 1
                                           : (r36 & 0x3F)) - cur_off);
        if (base) {
            *base = b;
        }
        if (set) {
            const int16_t delay = (int16_t)(b + *offset);
            if (delay < (int16_t)is_hs || delay > (is_hs ? 1024 : 63)) {
                return false;
            }
            memcpy(f, offset, sizeof *offset);
            cur_off = *offset;
            if (is_hs) {
                const uint16_t v = (uint16_t)(delay - 1);
                adv7511_write_register(0x35, (uint8_t)(v >> 2));
                r36 = (uint8_t)((r36 & 0x3F) | ((v & 0x03) << 6));
            } else {
                r36 = (uint8_t)((r36 & 0xC0) | delay);
            }
            adv7511_write_register(0x36, r36);
        }
    }
    *offset = cur_off;
    return true;
}
