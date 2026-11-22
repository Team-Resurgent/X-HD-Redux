// SPDX-License-Identifier: GPL-3.0-only
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

bool sd_aspect_known(xbox_encoder encoder) {
    bool wss_wide;
    return user_config_get()->aspect_ratio != ASPECT_RATIO_AUTO ||
           xbox_encoder_get_wss_widescreen(encoder, &wss_wide);
}

// mode_index (1-18, plus the standalone synthetics 0x13-0x15) -> timing profile, the unit of
// per-encoder hs/vs calibration (user_config's enc_cal[].profile[]). mode_indices that share a
// timing row share a profile; 0xFF = not a mode. Keep in sync with XHD_NUM_TIMING_PROFILES
// (the static assert checks the length).
// 0x01 (640-wide) and 0x02 (720-wide) get separate profiles, since the clock can't tell them
// apart reliably. 0x07 shares 0x08's profile 1 (on Xcalibur it uses 13 -- see
// xbox_get_video_timing()). Profile 15 is unused (an old slot, kept rather than
// renumbering).
static const uint8_t s_mode_index_to_profile[22] = {
    /* 0x00 */ 0xFF, // unused, mode_index is 1-18
    /* 0x01 */ 13,   // NTSC 640-wide VGA/SDTV (~24.4MHz) -- own profile, split from 0x02
    /* 0x02 */ 0,    // NTSC 720-wide SDTV -- own profile, split from 0x01
    /* 0x03 */ 2,    // PAL row 0 (case 0x03/0x04/0x05/0x06/0x10/0x12)
    /* 0x04 */ 3,    // PAL row 1
    /* 0x05 */ 4,    // PAL row 2
    /* 0x06 */ 5,    // PAL row 3
    /* 0x07 */ 1,    // NTSC SD 480p, 720-wide -- shares 0x08's profile
    /* 0x08 */ 1,    // NTSC SD 480p, 720-wide (shares profile w/ 0x07)
    /* 0x09 */ 8,    // 960x720 HD, its own case -- NOT part of the 480p group above
    /* 0x0A */ 9,    // 1280x720p (case 0x0A/0x0B)
    /* 0x0B */ 9,
    /* 0x0C */ 10,   // 1440x1080i, its own case
    /* 0x0D */ 11,   // 1920x1080i (case 0x0D/0x0E)
    /* 0x0E */ 11,
    /* 0x0F */ 17,   // NTSC 640-wide "1.1:1 PAR" -- own profile (base row shared with 0x01,
                      // see the 0x0F case-block comment)
    /* 0x10 */ 6,    // PAL row 4
    /* 0x11 */ 12,   // NTSC 480p "1.1:1 PAR": 640 framed from 0x08's line -- own profile
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

bool xbox_get_video_timing(xbox_encoder encoder, uint8_t mode_index, bool widescreen, XboxVideoTiming *timing) {
    if (!xbox_base_timing(encoder, mode_index, widescreen, timing)) return false;

    // 0x11 (AV_MODE_640x480_FPAR_TO_480P) runs 0x08's 720-wide 480p line -- the encoder
    // registers and the clock are 0x08's on all three encoders -- but the GPU scales the 640
    // picture to 654 pixels at 32-685 and leaves the rest black (FP_HVALID_START/END, read from
    // the GPU's registers on Xcalibur). 0x08's 720 window showed those borders as pillarbars, which
    // the analog output doesn't. 640 framed on the picture's centre: 7 picture pixels lost each
    // side. Conexant and Focus (same registers): the same picture, but its hsync two pixels
    // before 0x08's (735 vs 737), so the 640 starts two further in. Profile 12 tunes it per
    // encoder.
    // Exact (user_config sd_exact): Xcalibur's 640 modes on a CEA raster -- 0x0F/0x11 on 858x525,
    // 0x05/0x12 on 864x625, all at 27 MHz (GPU registers) -- go out as that CEA mode: a 720 window from 40
    // before the picture, the GPU's black evenly either side, CEA sync and VIC. The honest wire
    // VIC follows from the 720 shape and the clock. On screen the TV shows 480p/576p with bars both
    // sides. 0x11 is 0x08's line already: Exact just skips its 640 framing. The internal VIC
    // stays the Fill mode's: it's the arbiter's name for the mode (VIC 21/22 for 0x05/0x12), and
    // a changed one read as a different mode in standalone, "corrected" back every pass.
    // Conexant: only 0x11 is on a CEA raster (858x525 at 27 MHz); its other 640 modes run their
    // own clocks and line lengths (GPU registers). Focus (same): 0x11, and 0x05/0x12
    // on 0x06's 864x625 line at 27 MHz; its 0x0F/0x01 run 936 at 29.45 MHz, 0x03/0x10 22.68 MHz.
    const bool exact = user_config_get()->sd_exact &&
                       (mode_index == 0x11 ||
                        (encoder == ENCODER_XCALIBUR && (mode_index == 0x0F || mode_index == 0x05 || mode_index == 0x12)) ||
                        (encoder == ENCODER_FOCUS && (mode_index == 0x05 || mode_index == 0x12)));
    if (mode_index == 0x11) {
        timing->vic = widescreen ? VIC_03_480p_60_16_9 : VIC_01_VGA_640x480_4_3;
        // Focus's 0x11 picture starts two lines above 0x08's (checked on screen).
        if (encoder == ENCODER_FOCUS) timing->delay_vs -= 2;
    }
    // Conexant's and Focus's 0x11 line starts two pixels after 0x08's window (see above).
    const uint8_t cx_shift = encoder == ENCODER_XCALIBUR ? 0 : 2;
    if (mode_index == 0x11 && !exact) {
        timing->active_w = 640;
        timing->delay_hs += 39 + cx_shift;
    } else if (exact && mode_index == 0x11) {
        // The GPU's 654 picture at 32-685 of the 720 line. Conexant/Focus keep 0x08's window -- what
        // standalone sends for 0x11, so a handoff stays in place -- two pixels before the line,
        // so the picture sits two further in.
        timing->bar_l = 32 + cx_shift;
        timing->bar_r = 34 - cx_shift;
    } else if (exact && mode_index == 0x0F) {
        // 0x0F is 0x02's GPU raster -- 858x525, the same vertical, the 640 picture where 0x02's
        // 720 one is (GPU registers) -- so Exact sends 0x02's timing: what standalone sends for 0x0F (it
        // sees 0x02), and a 0x0F <-> 0x02 switch stays in place. 0x0F's own row has another
        // sync, which made a 0x0F <-> 0x02 switch a raster change with VIC 0 on the way.
        const uint8_t vic = timing->vic;
        xbox_base_timing(encoder, 0x02, widescreen, timing);
        timing->vic = vic;
        timing->bar_l = timing->bar_r = 40;
    } else if (exact && encoder == ENCODER_FOCUS) {
        // Focus 0x05/0x12 are 0x06's line with the 640 picture 40 in from 0x06's 720 one, so
        // Exact sends 0x06's timing, sync included -- a 0x05/0x12 <-> 0x06 switch stays in place.
        const uint8_t vic = timing->vic;
        xbox_base_timing(encoder, 0x06, widescreen, timing);
        timing->vic = vic;
        timing->bar_l = timing->bar_r = 40;
    } else if (exact) {
        // Xcalibur 0x05/0x12 (864x625): the 720 window from 40 before the picture, CEA 576p sync.
        timing->active_w = 720;
        timing->delay_hs -= 40;
        timing->bar_l = timing->bar_r = 40;
        timing->hsync_placement = 12;
        timing->hsync_duration = 64;
        timing->vsync_placement = 5;
        timing->vsync_duration = 5;
    }

    // 0x07 is 640-wide on Xcalibur: 0x01's row (xbox_base_timing()), profile 13.
    const bool xcal_07 = (encoder == ENCODER_XCALIBUR && mode_index == 0x07);
    const int enc = user_config_get_encoder_index(encoder);

    // The per-(profile, encoder) hs/vs calibration offset (user_config's profile_cal_t),
    // applied once. The profile is recorded only on success.
    s_last_profile_index = xcal_07 ? 13 : s_mode_index_to_profile[mode_index];

    // Bound-check: the table is hand-maintained, and an out-of-range profile would read into
    // the next encoder's slot. Out of range = no calibration.
    if (s_last_profile_index < XHD_NUM_TIMING_PROFILES) {
        const profile_cal_t *pc = &user_config_get()->enc_cal[enc].profile[s_last_profile_index];

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

    // Clocks only a PAL hybrid runs are PAL whatever the EEPROM says -- measured, so they
    // also hold with no capture (a board-only reset) and for PAL modes on an NTSC console
    // (the HD test app: Xcalibur 0x03/0x04 got the NTSC 0x08 row's sync placement).
    // Focus: no NTSC SD clock is below 24 MHz (they run 25.175-29.455), so a clock under
    // ARB_PAL50_SD_PCLK_LOW_HZ is the ~22.68 MHz PAL hybrid. Xcalibur: none below 24.40 MHz
    // (its NTSC 640 is ~24.545), where its PAL 20.8/23.4 MHz modes run.
    if (pclk_hz > 0 &&
        ((encoder == ENCODER_FOCUS && pclk_hz < ARB_PAL50_SD_PCLK_LOW_HZ) ||
         (encoder == ENCODER_XCALIBUR && pclk_hz < XCAL_STANDALONE_NTSC480_LOW_PCLK_MIN_HZ))) {
        pal_i_50 = true;
    }

    // Conexant: the gaps between its NTSC bands, [24.70, 26.70) and (27.45, 28.50) MHz, are
    // PAL by elimination. The upper gap stops below retail NTSC 480i (~29.454 MHz); no PAL SD
    // clock is above ~27.7 MHz. Interlaced content there was never captured, so with an
    // EEPROM capture the interlaced rule above keeps it.
    if (encoder == ENCODER_CONEXANT && pclk_hz > 0 && (!interlaced || !have_std)) {
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
                } else if (xbox_xcal_mode_index() == 0x07) {
                    mode_index = 0x07;   // 0x01's row, its own vertical start
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
