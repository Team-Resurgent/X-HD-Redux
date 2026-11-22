// SPDX-License-Identifier: GPL-3.0-only
// VIC autodetect/arbitration: the single VIC decision function. See vic_arbiter.h.
// Pure: no HAL/I2C/statics.
//
// decide_target_vic() walks a priority ladder top-down (L1 BIOS ... L6 blind fallback); the
// first level whose conditions hold returns, so a lower level never overrides a higher one.

#include "vic_arbiter.h"
#include "adv7511_vic.h"
#include "types.h"
#include "xbox_mode_index.h"

// "Is the field rate below ~57 Hz", assuming a dense NTSC 858x525 frame: too slow to be 60 Hz
// even in full NTSC timing, so a 50 Hz signal. A rate check, not a colour-standard check.
// Unreliable for narrow NTSC (~24.5-25.2 MHz: an 800x525 frame really runs 60 Hz but computes
// < 57 here), so it is only the last resort when the region is unknown.
static bool arb_signal_looks_sub57hz(uint32_t pclk_hz) {
    if (pclk_hz == 0) return false;
    return ((pclk_hz * 10u) / 45045u) < 5700u;   // < 57.00 Hz
}

// Which raster conventions apply (line count / field rate / Path A), not the colour standard:
//  - ARB_REGION_PAL50: a 50 Hz PAL raster. The only value that enables Path A and the 576
//    blind fallback.
//  - ARB_REGION_NTSC: a 525-line 60 Hz raster -- NTSC-M/J, PAL-M, and a PAL-I console set to
//    60 Hz (EEPROM keeps the PAL-I standard but sets the 60 Hz bit).
//  - ARB_REGION_UNKNOWN: no EEPROM standard captured.
// arb_region() reads it from the EEPROM.
typedef enum { ARB_REGION_UNKNOWN = 0, ARB_REGION_NTSC, ARB_REGION_PAL50 } arb_region_t;

// Line counts, as the line-count evidence in arb_assess() narrows them: 480 (640 and 720
// wide alike -- the detector can't tell them apart), 576, HD. A family's bit; none for FAM_NONE.
#define ARB_LINES_480 1u
#define ARB_LINES_576 2u
#define ARB_LINES_HD  4u

// Defined further down; arb_blind_fallback_vic() needs them first.
static void arb_resolve_aspect(mode_family_t fam, const arb_snapshot_t *in, int bios_ws,
                               bool *widescreen, bool *anamorphic);
static uint8_t arb_vic_for(mode_family_t fam, bool widescreen);

// --- PAL50 SD: which SD raster a PAL50 pixel clock is ---
// The line detector handles PAL50 SD badly (reads 576p50 as VIC 17/18 but goes blind or
// sticks on a stale VIC for the 480-line modes), so the clock decides instead. Clocks come
// from the encoder, so each encoder has its own measured bands:
//  - Focus (the original calibration): 640/720x480@50 (0x03/0x04, ~22.68 MHz) sit below this
//    function's floor and belong to Path A / the width signal; 576p50 (0x06) runs at the
//    standard 27.000 MHz.
//  - Conexant: 24.9 MHz (640 hybrid, 0x03), 27.562 MHz (720 hybrid, 0x04/0x10), 27.125 MHz
//    (640x576, 0x12), 27.687 MHz (576p50, 0x06).
//  - Xcalibur: 20.8 MHz (0x03), 23.4 MHz (0x04/0x10), and 27.000 MHz shared by 480- and
//    576-line modes -- decided by the register mode_index instead.
// ARB_PAL50_SD_PCLK_LOW_HZ is in vic_arbiter.h (shared with xbox_video_standalone.c).
#define ARB_PAL50_SD_PCLK_HIGH_HZ   28500000UL   // Focus-measured. ceiling
#define ARB_PAL50_640_PCLK_MAX_HZ   26500000UL   // Focus-measured. < this (in band) -> 640x480@50 (matches width WIDE thresh)
#define ARB_PAL50_576P_PCLK_MIN_HZ  26800000UL   // Focus-measured. >= this
// Conexant's 576p floor: between its highest 480-line clock (27.562 MHz) and its 576p50 clock
// (27.687 MHz), clear of the ~1 kHz capture-to-capture noise. Focus's 26.8 MHz floor would put
// Conexant's 480-line hybrids on the 576 side.
#define ARB_CONEXANT_PAL576_PCLK_MIN_HZ   27600000UL   // Conexant-measured.

// Conexant's two narrow-576 clocks (see ARB_CX_640x576_* in vic_arbiter.h).
static __attribute__((noinline)) bool arb_cx_clock_is_narrow_576(uint32_t p) {   // noinline: -64 B
    return arb_cx_clock_is_640x576(p) || arb_cx_clock_is_640x576_par(p);
}

static uint8_t arb_pal50_sd_vic(const arb_snapshot_t *in) {
    if (!in->pclk_stable) {
        return VIC_00_VIC_Unavailable;
    }
    // Xcalibur: its narrow PAL clocks (20.8/23.4 MHz) are below the Focus/Conexant floor, and
    // 27.000 MHz carries 480- and 576-line modes alike, so the register mode_index decides.
    // Both directions matter: this result also overrides the detector's family at Level 4, so
    // a wide 480-line index answering "narrow" here would demote a correct VIC 3 to VIC 1.
    // Known-narrow indices are VIC 1; known-wide ones defer to the measured width.
    if (in->encoder_id == ENCODER_XCALIBUR) {
        const uint8_t m = xbox_mode_flags(in->xcal_mode_index, ENCODER_XCALIBUR);
        const mode_family_t f = (mode_family_t)(m & MODE_FAM_MASK);
        if (f == FAM_SD_720x576) {
            return (m & MODE_640) ? VIC_21_576i_50_2xClk_4_3 : VIC_18_576p_50_16_9;
        }
        if (f == FAM_SD_640) return VIC_01_VGA_640x480_4_3;
        if (f == FAM_SD_720x480) {
            return in->width_wide ? VIC_02_480p_60__4_3 : VIC_01_VGA_640x480_4_3;
        }
        // No mode set seen yet: width + clock for the narrow case; anything else falls through
        // to the generic clock split below.
        if (in->width_confirmed && !in->width_wide) {
            return (in->pclk_hz < 25000000UL) ? VIC_01_VGA_640x480_4_3 : VIC_21_576i_50_2xClk_4_3;
        }
    }
    // Focus: ~22.68 MHz is the 480-line PAL-I hybrid (0x03/0x04/0x10), below the floor. Path A
    // is Xcalibur-only, so nothing else classified it, and L6's 50 Hz rule guessed 576: 640x480
    // content went out as VIC 21/22. The width splits 640/720.
    if (in->encoder_id == ENCODER_FOCUS && in->width_confirmed &&
        in->pclk_hz >= ARB_PATH_A_PCLK_LOW_HZ && in->pclk_hz < ARB_PATH_A_PCLK_HIGH_HZ) {
        return in->width_wide ? VIC_02_480p_60__4_3 : VIC_01_VGA_640x480_4_3;
    }
    if (in->pclk_hz < ARB_PAL50_SD_PCLK_LOW_HZ ||
        in->pclk_hz > ARB_PAL50_SD_PCLK_HIGH_HZ) {
        return VIC_00_VIC_Unavailable;
    }
    if (in->encoder_id == ENCODER_CONEXANT && in->width_confirmed && !in->width_wide) {
        if (arb_cx_clock_is_narrow_576(in->pclk_hz)) {
            return VIC_21_576i_50_2xClk_4_3;
        }
        // Width confirmed narrow and not a 576 clock: every other narrow Conexant PAL mode
        // (0x01/0x03/0x0F/0x10) is 480-line. The clock bands below can't decide this -- 0x10's
        // 27.562 MHz coincides with the wide 0x02/0x04 clock.
        return VIC_01_VGA_640x480_4_3;
    }
    // Conexant, width confirmed wide, below its 576p floor: the 720-wide 480-line rasters
    // (PAL 720x480 ~27.563 MHz, PAL-60 720x480 ~27.378 MHz). Without this the band returned
    // VIC 0, the blind hold never resolved, and every blind stretch ended in the
    // ARB_HOLD_STUCK_TIMEOUT_MS escape.
    if (in->encoder_id == ENCODER_CONEXANT && in->width_confirmed && in->width_wide &&
        !in->width_stale_held && in->pclk_hz >= ARB_PAL50_640_PCLK_MAX_HZ &&
        in->pclk_hz < ARB_CONEXANT_PAL576_PCLK_MIN_HZ) {
        return VIC_02_480p_60__4_3;
    }
    // Focus narrow at its 576 clock (27.000 MHz) is the 640x576 pair (0x05/0x12); its 480-line
    // narrow modes run 22.68 MHz (above).
    if (in->encoder_id == ENCODER_FOCUS && in->width_confirmed && !in->width_wide &&
        in->pclk_hz >= ARB_PAL50_576P_PCLK_MIN_HZ) {
        return VIC_21_576i_50_2xClk_4_3;
    }
    if (in->pclk_hz < ARB_PAL50_640_PCLK_MAX_HZ)     return VIC_01_VGA_640x480_4_3;    // 640x480@50
    // Conexant needs its own 576p floor (see ARB_CONEXANT_PAL576_PCLK_MIN_HZ): with Focus's,
    // its 27.562 MHz 480-line hybrid read as 576p and flapped VIC 3 <-> VIC 18 every ~800 ms.
    const uint32_t floor_576p = (in->encoder_id == ENCODER_CONEXANT)
                              ? ARB_CONEXANT_PAL576_PCLK_MIN_HZ
                              : ARB_PAL50_576P_PCLK_MIN_HZ;
    if (in->pclk_hz >= floor_576p)                        return VIC_18_576p_50_16_9;       // 576p50
    return VIC_00_VIC_Unavailable;   // between the 640 ceiling and the 576p floor --
                                      // doesn't confidently match any profiled point; caller
                                      // keeps whatever it had rather than guessing
}

// arb_pal50_sd_vic(), debounced: the verdict must hold for ARB_PAL50_SD_HOLD_MS with no TMDS
// cycle running before it drives a switch -- otherwise a clock near a split point, or the
// CTS drop of a gate cycle, flips 576p50 <-> hybrid, and each flip's own cycle feeds the next.
// VIC 0 until settled. Callers that only decide whether to HOLD use the raw verdict.
static uint8_t arb_pal50_sd_vic_debounced(const arb_snapshot_t *in, arb_state_t *st) {
    uint8_t raw = in->tmds_cycle_busy ? VIC_00_VIC_Unavailable : arb_pal50_sd_vic(in);
    if (raw != st->pal50_sd_cand) {
        st->pal50_sd_cand = raw;
        st->pal50_sd_since = in->now_ms;
        return VIC_00_VIC_Unavailable;
    }
    if ((in->now_ms - st->pal50_sd_since) < ARB_PAL50_SD_HOLD_MS) return VIC_00_VIC_Unavailable;
    return raw;
}

// L6's SD guess while the detector is blind, strongest evidence first:
//  0. the line-count evidence (arb_assess()) leaves only 576 lines -> 576, whatever the region:
//     it is measured (Xcalibur's mode index, Conexant's narrow-576 clocks, Focus narrow at
//     27.000 MHz), and a missed EEPROM capture put a Focus 640x576 out as VIC 1.
//  1. width confirmed narrow, not PAL50 -> a real 640-wide NTSC signal: VIC 1.
//  2. width confirmed wide -> 720x480, or 720x576 when the PAL50 clock says 576. The sticky
//     width verdict beats the clock here: this often runs while a previous apply has just
//     disturbed the clock, and a momentary 0 would otherwise read "not 50 Hz" and undo the
//     width decision (L5 and L6 chased each other 640 <-> 720 on blind PAL content).
//  3. narrow and the PAL50 clock says VIC 1 -> VIC 1.
//  4. a 50 Hz signal (region not NTSC and the rate estimate says < 57 Hz, or the PAL50 clock
//     says narrow 576i) -> 576: VIC 21/22 if narrow, else VIC 17/18.
//  5. otherwise VIC 1.
// On a known-NTSC console only the measured case (0) returns a 576 VIC. *widescreen /
// *anamorphic come from arb_resolve_aspect(), so the caller can pass them straight to arb_pick().
static __attribute__((noinline)) uint8_t arb_blind_fallback_vic(const arb_snapshot_t *in, arb_region_t region,
                                      uint8_t lines, bool *widescreen, bool *anamorphic) {
    *widescreen = false;
    *anamorphic = false;
    // The raw PAL50 verdict, not the debounced one: the debounce resets on every TMDS cycle,
    // and every commit here causes one, so it would self-oscillate. This is a deterministic
    // function of clock and width; the blind_fb latch in decide_target_vic() absorbs
    // transients.
    uint8_t pal50_raw = arb_pal50_sd_vic(in);
    const bool only_576 = (lines & (uint8_t)~ARB_LINES_HD) == ARB_LINES_576;
    // Not on PAL50: there "confirmed narrow" can't tell a PAL 640x576 (Conexant 0x05) from an
    // NTSC 640x480, so PAL50-narrow falls through to the 50 Hz branch below. The shared tail
    // resolves the aspect the same way the apply path does (4:3, or anamorphic 16:9), so the
    // arbiter and the apply agree and L6 doesn't reapply VIC 1 every loop.
    if (in->width_confirmed && !in->width_wide && region != ARB_REGION_PAL50 && !only_576) {
        goto narrow_640;
    }
    if (in->width_confirmed && in->width_wide) {
        // 576 only on the PAL50 clock's 576 side. A Conexant clock at/above its 576p floor is
        // 576 even mid-cycle, when pclk_stable (and so pal50_raw) is briefly false.
        bool is_576 = only_576 || (region == ARB_REGION_PAL50 &&
            (pal50_raw == VIC_18_576p_50_16_9 ||
             (in->encoder_id == ENCODER_CONEXANT && in->pclk_hz >= ARB_CONEXANT_PAL576_PCLK_MIN_HZ)));
        mode_family_t fam = is_576 ? FAM_SD_720x576 : FAM_SD_720x480;
        bool ana;
        arb_resolve_aspect(fam, in, -1, widescreen, &ana);
        return arb_vic_for(fam, *widescreen);
    }
    bool narrow = in->width_confirmed && !in->width_wide;
    if (narrow && pal50_raw == VIC_01_VGA_640x480_4_3 && !only_576) {
        goto narrow_640;
    }
    // The rate check runs on PAL50 too: the EEPROM region is PAL50 all boot, and a
    // PAL console's own 60 Hz modes (24.4/27.4 MHz) must not be forced onto 50 Hz timings.
    // The PAL50 clock's narrow-576i verdict is OR'd in because Conexant's narrow-576 clocks
    // (24.625, 27.125 MHz) sit between narrow-480 ones (24.420/24.937, 26.874/27.562 MHz) --
    // a single rate cutoff can't express that.
    bool is_50hz = only_576 || (region != ARB_REGION_NTSC && arb_signal_looks_sub57hz(in->pclk_hz)) ||
                   (narrow && pal50_raw == VIC_21_576i_50_2xClk_4_3);
    if (is_50hz) {
        // Best guess 576-line; the aspect still comes from a real source (WSS/EEPROM/user).
        bool ana;
        arb_resolve_aspect(FAM_SD_720x576, in, -1, widescreen, &ana);
        if (narrow) return *widescreen ? VIC_22_576i_50_2xClk_16_9 : VIC_21_576i_50_2xClk_4_3;
        return *widescreen ? VIC_18_576p_50_16_9 : VIC_17_576p_50__4_3;
    }
narrow_640:
    arb_resolve_aspect(FAM_SD_640, in, -1, widescreen, anamorphic);
    return VIC_01_VGA_640x480_4_3;
}

// mode_index facts (family, 640-wide, PAL, BIOS-authoritative) are xbox_mode_index.h's table.
// (Neither flag below depends on the encoder.)
static bool arb_is_bios_authoritative(uint8_t idx) {
    return (xbox_mode_flags(idx, 0) & MODE_BIOS_AUTH) != 0;
}

// VIC for a family + aspect. 640-wide has no 16:9 VIC: it stays VIC 1 and the 16:9 goes to
// the AVI only (anamorphic).
static uint8_t arb_vic_for(mode_family_t fam, bool widescreen) {
    switch (fam) {
        case FAM_SD_640:      return VIC_01_VGA_640x480_4_3;
        case FAM_SD_720x480:  return widescreen ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3;
        case FAM_SD_720x576:  return widescreen ? VIC_18_576p_50_16_9 : VIC_17_576p_50__4_3;
        case FAM_HD_720p:     return VIC_04_720p_60_16_9;
        case FAM_HD_1080i:    return VIC_05_1080i_60_16_9;
        default:              return VIC_00_VIC_Unavailable;
    }
}

// A hold timer: the first call starts it (*since = now) and never fires; later calls fire once
// `ms` have passed. *since == 0 means "not started" -- the callers clear it to re-arm.
static bool held_for(uint32_t *since, uint32_t now, uint32_t ms) {
    if (*since == 0) {
        *since = now;
        return false;
    }
    return now - *since >= ms;
}

static mode_family_t arb_family_of_vic(uint8_t vic) {
    switch (vic) {
        case VIC_01_VGA_640x480_4_3:                              return FAM_SD_640;
        case VIC_02_480p_60__4_3: case VIC_03_480p_60_16_9:       return FAM_SD_720x480;
        case VIC_17_576p_50__4_3: case VIC_18_576p_50_16_9:
        case VIC_21_576i_50_2xClk_4_3: case VIC_22_576i_50_2xClk_16_9: return FAM_SD_720x576;
        case VIC_04_720p_60_16_9:                                 return FAM_HD_720p;
        case VIC_05_1080i_60_16_9:                                return FAM_HD_1080i;
        default:                                                  return FAM_NONE;
    }
}

// The console's EEPROM VideoStandard, or UNKNOWN when it wasn't captured (it is only broadcast
// at console boot, so a board-only reset loses it; a console reboot recaptures it).
static arb_region_t arb_region(const arb_snapshot_t *in) {
    if (in->eeprom_std_valid) {
        // A PAL-I console set to 60 Hz runs NTSC-shaped 525-line rasters: NTSC here.
        return (in->eeprom_std_is_pal && !in->eeprom_refresh_60hz)
                   ? ARB_REGION_PAL50 : ARB_REGION_NTSC;
    }
    return ARB_REGION_UNKNOWN;
}

static bool arb_vic_is_sd(uint8_t vic) {
    mode_family_t f = arb_family_of_vic(vic);
    return f == FAM_SD_640 || f == FAM_SD_720x480 || f == FAM_SD_720x576;
}

// hw_vic "agrees" with active_vic when the geometry family matches; aspect belongs to the
// width/user, so VIC 2 (hw) vs VIC 3 (active) is agreement.
static bool arb_family_agrees(uint8_t hw_vic, uint8_t active_vic) {
    mode_family_t h = arb_family_of_vic(hw_vic), a = arb_family_of_vic(active_vic);
    if (h == FAM_NONE || a == FAM_NONE) return false;
    if (h == a) return true;
    // 640 and 720x480 count as one "480-line SD" family: the detector reads a real narrow
    // 480 raster as VIC 2, and a settled VIC 1 must not be promoted to VIC 2 every 30 ms.
    if ((h == FAM_SD_640 || h == FAM_SD_720x480) &&
        (a == FAM_SD_640 || a == FAM_SD_720x480)) return true;
    return false;
}

// The AVI aspect for a family. Priority: HD (always 16:9, even over a user 4:3) > user
// setting > the BIOS packet's widescreen bit (bios_ws >= 0, BIOS-driven families only) >
// live encoder aspect (enc_ws_valid / enc_widescreen) > EEPROM Widescreen > 4:3.
// *anamorphic is set when the family is 640-wide but 16:9 is wanted.
static void arb_resolve_aspect(mode_family_t fam, const arb_snapshot_t *in, int bios_ws,
                               bool *widescreen, bool *anamorphic) {
    bool want_wide;
    if (mode_family_is_hd(fam))                    want_wide = true;   // 720p/1080i are 16:9-only
    else if (in->user_aspect == ARB_ASPECT_16_9) want_wide = true;
    else if (in->user_aspect == ARB_ASPECT_4_3)  want_wide = false;
    else if (bios_ws >= 0)                        want_wide = (bios_ws != 0);
    else if (fam != FAM_NONE) {
        // All SD families resolve the same way. The live encoder aspect is rewritten per
        // mode/title, so it beats the EEPROM bit (read once at boot); the EEPROM bit is the
        // same one sd_resolve_widescreen() uses on the apply side, so both agree. Deliberately
        // no width or detector fallback: wide-means-16:9 was wrong on a Widescreen=N console,
        // and the detector's VIC 17-vs-18 just echoes our own 0x17[1] write.
        want_wide = in->enc_ws_valid ? in->enc_widescreen
                  : in->eeprom_ws_valid ? in->eeprom_widescreen : false;
    }
    else                                         want_wide = false;
    *anamorphic = (fam == FAM_SD_640) && want_wide;
    *widescreen = want_wide && !*anamorphic;
}

static vic_decision_t arb_hold(void) {
    vic_decision_t d = {0};
    d.source = ARB_SRC_HOLD;
    d.apply = false;
    return d;
}

static vic_decision_t arb_pick(arb_source_t src, uint8_t vic, bool ws, bool ana,
                               const arb_snapshot_t *in) {
    vic_decision_t d = {0};
    d.source = src;
    d.target_vic = vic;
    d.widescreen = ws;
    d.anamorphic = ana;
    // Apply on any VIC change (VIC pairs 2/3, 17/18, 21/22 also cover aspect), plus an
    // aspect-only change on VIC 1, whose 4:3 and anamorphic 16:9 share one VIC.
    d.apply = (vic != in->active_vic) ||
              (vic == VIC_01_VGA_640x480_4_3 && ana != in->active_is_anamorphic);
    return d;
}

static uint8_t arb_lines_of(mode_family_t f) {
    return f == FAM_SD_640 || f == FAM_SD_720x480 ? ARB_LINES_480 : f == FAM_SD_720x576 ? ARB_LINES_576 :
           mode_family_is_hd(f) ? ARB_LINES_HD : 0;
}

// The narrow-576 pick: VIC 21/22, the placeholder pair the console itself uses for its
// 640x576 modes (CEA has no 640-wide 576-line VIC, so arb_vic_for() can't express it).
static vic_decision_t arb_pick_576i(arb_source_t src, const arb_snapshot_t *in) {
    bool ws, ana;
    arb_resolve_aspect(FAM_SD_720x576, in, -1, &ws, &ana);
    return arb_pick(src, ws ? VIC_22_576i_50_2xClk_16_9 : VIC_21_576i_50_2xClk_4_3, ws, ana, in);
}

// Resolve a family's aspect and pick its CEA VIC.
static vic_decision_t arb_pick_family(arb_source_t src, mode_family_t fam, const arb_snapshot_t *in) {
    bool ws, ana;
    arb_resolve_aspect(fam, in, -1, &ws, &ana);
    return arb_pick(src, arb_vic_for(fam, ws), ws, ana, in);
}

// Reset the blind-fallback latch: when the detector stops being blind (L6), or when another
// level makes a real decision (decide_target_vic()).
static void arb_clear_blind_fallback(arb_state_t *st) {
    st->blind_fb_valid = false;
    st->blind_fb_vic = 0;
    st->blind_fb_ws = false;
    st->blind_fb_ana = false;
    st->blind_fb_aspect = ARB_ASPECT_AUTO;
    st->blind_fb_cand_vic = 0;
    st->blind_fb_cand_ws = false;
    st->blind_fb_cand_ana = false;
    st->blind_fb_cand_since = 0;
    st->blind_since = 0;
    st->hold_stuck_since = 0;
    st->hold_stuck_escaped = false;
}


// Per-decision values shared by the levels (see arb_assess()).
typedef struct {
    bool         blind;               // detector reads VIC 0
    bool         effective_blind;     // blind, or a reading that can't be trusted
    bool         hw_reading_current;  // the reading describes the current raster
    bool         sd_clock_confirmed;  // a settled SD-range clock
    arb_region_t region;
    uint8_t      pal50_dbnc;          // debounced PAL50 verdict (VIC 0 = none)
    bool         narrow_576;          // 640x576 known: a Conexant narrow-576 clock, Xcalibur's mode index, Focus narrow 27 MHz
    bool         hw_narrow_576;       // a 576 reading on confirmed-narrow content
    uint8_t      lines;               // ARB_LINES_* the raster can be (line-count evidence)
    uint8_t      bios_lines;          // the same for L1 staleness (2): without a width or index
                                      // from before the console's latest packet
    bool         hw_ruled_out;        // the reading's family is outside `lines`
} arb_ctx_t;

// A level's decision: store it and report "decided".
static bool arb_done(vic_decision_t *out, vic_decision_t d) {
    *out = d;
    return true;
}

// Everything the levels share, computed once per decision and in this order (some of it
// updates st): which detector readings to distrust, the region, the debounced PAL50 verdict,
// and whether the detector reading describes the current raster.
static void arb_assess(const arb_snapshot_t *in, arb_state_t *st, arb_ctx_t *c) {
    const bool blind = (in->hw_vic == VIC_00_VIC_Unavailable);

    // --- Detector readings that can't be trusted; each falls through to L3/L6 as if blind ---

    // hw_stale: 0x3E can freeze on a stale VIC while the clock moves elsewhere; L4 would hold
    // on it forever since hw_vic never changes. Baseline the clock when the raw byte changes;
    // a departure over ARB_HW_DRIFT_HZ sustained for ARB_HW_DRIFT_HOLD_MS marks it
    // stale (a real mode change moves the clock by MHz). Threshold sized from Focus jitter;
    // this and the 576 reconciliation below were measured on Focus and Conexant only.
    bool hw_stale = false;
    if (!blind) {
        // Conexant 640x576 (0x05, ~24.6 MHz) and 720x576 (0x06, ~27.7 MHz) both read VIC 17:
        // the same raw byte across a real ~3 MHz jump. Re-baseline whenever the clock confirms
        // a genuine 576p reading instead of calling it drift.
        bool cx_valid_576 = in->encoder_id == ENCODER_CONEXANT &&
                            arb_family_of_vic(in->hw_vic) == FAM_SD_720x576 &&
                            in->pclk_hz >= ARB_CONEXANT_PAL576_PCLK_MIN_HZ;
        if (in->hw_vic != st->hw_vic_locked || cx_valid_576) {
            st->hw_vic_locked = in->hw_vic;
            st->hw_vic_lock_pclk = in->pclk_hz;
            st->hw_vic_drift_since = 0;
        } else if (!in->tmds_cycle_busy && in->pclk_hz > 0 && st->hw_vic_lock_pclk > 0) {
            uint32_t drift = (in->pclk_hz > st->hw_vic_lock_pclk)
                            ? (in->pclk_hz - st->hw_vic_lock_pclk)
                            : (st->hw_vic_lock_pclk - in->pclk_hz);
            if (drift > ARB_HW_DRIFT_HZ) {
                if (held_for(&st->hw_vic_drift_since, in->now_ms, ARB_HW_DRIFT_HOLD_MS))
                    hw_stale = true;
            } else {
                st->hw_vic_drift_since = 0;
            }
        }
    } else if (in->pclk_hz == 0) {
        // Blind with no clock at all: nothing up to hold a reading of, so the next one is a
        // fresh acquisition -- re-baseline it. A wrong 17 at 22.68 MHz otherwise made the
        // real 17 on the next mode's 27.000 MHz 576 raster "drift", and L5 put 720x480 out
        // ahead of it (seen on Focus). A reading held through the gap is not blind.
        st->hw_vic_locked = VIC_00_VIC_Unavailable;
    }

    // All encoders: the detector reads line timing, so a narrow mode on a 720x480-shaped
    // raster still reads VIC 2. When the encoder's own width signal is confirmed and
    // disagrees with the 480 family's width, trust the width (seen on Xcalibur).
    // Narrow 576 is not here: hw_narrow_576 keeps the reading's correct line count.
    const bool hw_width_mismatch = in->width_confirmed &&
        ((in->width_wide && arb_family_of_vic(in->hw_vic) == FAM_SD_640) ||
         (!in->width_wide && arb_family_of_vic(in->hw_vic) == FAM_SD_720x480));

    // hw_narrow_576: a 576-family reading (CEA: 720-wide only) on content the width signal
    // confirms narrow. The line count is right and only the width is wrong -- the console's
    // own 640x576 modes (0x05/0x12) -- so L4 sends it to the VIC 21/22 placeholder instead of
    // discarding it. (Discarding it gave Focus a 480-line window on 576-line content, "bottom
    // cut off"; Conexant 0x05 stuck on VIC 17.)
    // Conexant: only at its two calibrated narrow-576 clocks -- an allow-list can't have the
    // gaps an exclude-list did (0x10 at 27.562 MHz is narrow 480-line). It also keeps a width
    // verdict still lagging from the previous mode (0x05 -> 0x06) from forcing VIC 21.
    const bool cx_576_narrow = arb_cx_clock_is_narrow_576(in->pclk_hz);
    // Xcalibur: the same 27.000 MHz serves several modes, so the register mode_index decides.
    // A stale 576 reading from 0x12 otherwise forced VIC 21 on the next mode (0x01).
    const uint8_t xm = xbox_mode_flags(in->xcal_mode_index, ENCODER_XCALIBUR);
    const bool xcal_576_narrow = (xm & MODE_FAM_MASK) == FAM_SD_720x576 && (xm & MODE_640);
    // Focus at its 576 clock (27.000 MHz): narrow is only ever 640x576 (0x05/0x12), and the
    // SDTV path only ever 576 lines (0x05/0x06/0x12) -- its 480p modes share the clock but are
    // 720-wide on the progressive path, and its narrow 480-line ones run 22.68 (PAL-60) or
    // 29.455 MHz (NTSC). Measured, so it holds without the EEPROM region too, and for PAL
    // modes on an NTSC console (0x06 went out 720x480).
    const bool focus_576_clock = in->encoder_id == ENCODER_FOCUS &&
        in->pclk_hz >= ARB_PAL50_576P_PCLK_MIN_HZ && in->pclk_hz <= ARB_PAL50_SD_PCLK_HIGH_HZ;
    const bool focus_576_narrow = focus_576_clock && in->width_confirmed && !in->width_wide &&
        !in->width_stale_held;
    const bool hw_narrow_576 =
        in->width_confirmed && !in->width_wide && arb_family_of_vic(in->hw_vic) == FAM_SD_720x576 &&
        ((in->encoder_id != ENCODER_CONEXANT && in->encoder_id != ENCODER_XCALIBUR) ||
         cx_576_narrow || xcal_576_narrow);

    // --- Line-count evidence: the families the raster can be, from all but the detector ---
    // Each fact narrows the set; a reading outside it is stale, ahead of its raster or misread,
    // and counts as blind (L3/L4/L6) and as no evidence of a mode change (L1 staleness (1)):
    //  - a settled HD clock is HD (no SD mode runs above ~29.5 MHz: a stale VIC 2 on a frozen
    //    boot into 720p put 720x480 out for good on Focus); a settled SD clock is SD
    //    (the detector lingers on HD ~2 s after 720p -> 640x480 on Xcalibur);
    //  - Xcalibur's register mode index names the line count (1080i went out 25 ms into the
    //    next mode set's burst; a stale 17 on NTSC 480p went out as 720x576);
    //  - an NTSC console never outputs 576 lines (the reading only, below), and none runs above
    //    ARB_PAL50_SD_PCLK_HIGH_HZ (measured SD bursts run 24.6-27.7 MHz; a stray 17 on Focus
    //    PAL-60 0x02 at 29.455 MHz went out as 720x576, and released the BIOS 16:9);
    //  - Conexant runs 576 lines only at its two narrow-576 clocks and in its 576p band, and
    //    only 576 there: ~1 in 3 settled 576 readings is VIC 2, and wide content in the band
    //    went out as VIC 3 ahead of L4's VIC 18;
    //  - Focus/Xcalibur in the Path A window ([22.5, 23.95) MHz): the PAL-I 480-line hybrid
    //    (576p50 runs ~27 MHz, 576i50 ~13.5 MHz);
    //  - Focus at its 576 clock, narrow or on the SDTV path: 576 (see focus_576_clock).
    uint8_t lines = ARB_LINES_480 | ARB_LINES_576 | ARB_LINES_HD;
    uint8_t width_lines = lines;   // the facts from the width signal / Xcalibur index
    if (in->pclk_stable && in->pclk_hz > 0) {
        lines &= in->pclk_hz >= ARB_HD_PCLK_MIN_HZ ? ARB_LINES_HD : (uint8_t)~ARB_LINES_HD;
    }
    if ((xm & MODE_FAM_MASK) != FAM_NONE) width_lines &= arb_lines_of((mode_family_t)(xm & MODE_FAM_MASK));
    if (in->pclk_hz > ARB_PAL50_SD_PCLK_HIGH_HZ ||
        ((in->encoder_id == ENCODER_FOCUS || in->encoder_id == ENCODER_XCALIBUR) &&
         in->pclk_hz >= ARB_PATH_A_PCLK_LOW_HZ && in->pclk_hz < ARB_PATH_A_PCLK_HIGH_HZ)) {
        lines &= (uint8_t)~ARB_LINES_576;
    }
    if (focus_576_narrow) width_lines &= ARB_LINES_576;
    if (focus_576_clock && in->focus_sdtv) lines &= ARB_LINES_576;
    if (in->encoder_id == ENCODER_CONEXANT && in->pclk_hz > 0 && in->pclk_hz <= ARB_PAL50_SD_PCLK_HIGH_HZ) {
        lines &= (cx_576_narrow || in->pclk_hz >= ARB_CONEXANT_PAL576_PCLK_MIN_HZ)
               ? ARB_LINES_576 : (uint8_t)~ARB_LINES_576;
    }
    // A width or index from before the console's latest packet describes the mode set before
    // it (one whose writes the sniffer missed): it can't rule the packet's own mode out -- it
    // replaced a correct BIOS 0x07 with 640x576 on Focus, 0x05 with 640x480 on Xcalibur. The
    // arbiter's own levels still use it.
    const uint8_t bios_lines = in->width_predates_bios ? lines : (uint8_t)(lines & width_lines);
    lines &= width_lines;
    // The EEPROM standard judges the reading only: the HD test app puts PAL modes out on an
    // NTSC console too, so the BIOS packet's own mode stays possible (L1 staleness (2) dropped
    // a forced 0x05's 640x576 for VIC 1 on Conexant).
    const uint8_t hw_lines = (in->eeprom_std_valid && !in->eeprom_std_is_pal) ? (uint8_t)(lines & ~ARB_LINES_576) : lines;
    const bool hw_ruled_out = (arb_lines_of(arb_family_of_vic(in->hw_vic)) & ~hw_lines) != 0;

    // And a reading in no family at all (Conexant's 625-line rasters read VIC 29/30/37/38, the
    // pixel-repeated 576p codes): no level acted on it, so an Aspect change waited for the
    // detector to go blind (a 16:9 -> 4:3 change came 0.4 s late on Conexant PAL), and a
    // frozen boot on it applied nothing at all.
    const bool effective_blind = blind || hw_stale || hw_width_mismatch || hw_ruled_out ||
        arb_family_of_vic(in->hw_vic) == FAM_NONE;

    // A settled SD-range clock outranks a flapping detector: on a rock-steady ~21 MHz
    // 480-line/50 Hz hybrid, 0x3E cycles 720p <-> blind <-> 480p about every second.
    const bool sd_clock_confirmed = in->pclk_stable && in->pclk_hz > 0 &&
                                    in->pclk_hz < ARB_HD_PCLK_MIN_HZ;

    // Region, resolved once. NTSC makes Path A (L3) and every PAL fallback unreachable.
    const arb_region_t region = arb_region(in);

    // The debounced PAL50 verdict (see arb_pal50_sd_vic_debounced()), used by L4 and L6.
    // Computed while the region is still UNKNOWN too: a PAL clock here can be the first PAL
    // evidence, and waiting for the region would cost an iteration.
    uint8_t pal50_dbnc = VIC_00_VIC_Unavailable;
    if (region != ARB_REGION_NTSC) {
        pal50_dbnc = arb_pal50_sd_vic_debounced(in, st);
    } else {
        st->pal50_sd_cand = VIC_00_VIC_Unavailable;
        st->pal50_sd_since = 0;
    }

    // The clock when the detector's line-count FAMILY last changed (not its raw byte: 0x3E
    // flips between aspect siblings like 17/18 whenever 0x17[1] is rewritten). A family change
    // seen with no clock is baselined on the first real clock after it.
    if (!blind) {
        const uint8_t fam = (uint8_t)arb_family_of_vic(in->hw_vic);
        if (fam != st->hw_fam_locked) {
            st->hw_fam_locked = fam;
            st->hw_fam_lock_pclk = in->pclk_hz;
        } else if (st->hw_fam_lock_pclk == 0 && in->pclk_hz > 0) {
            st->hw_fam_lock_pclk = in->pclk_hz;
        }
    }
    // The reading describes the current raster only while the clock is still the one its
    // family locked at -- 0x3E can keep the previous raster's family for seconds. Used by
    // L1's staleness case (1) and L4's settle gate.
    const uint32_t hw_lock_drift = (in->pclk_hz > st->hw_fam_lock_pclk)
                                   ? (in->pclk_hz - st->hw_fam_lock_pclk)
                                   : (st->hw_fam_lock_pclk - in->pclk_hz);
    const bool hw_reading_current = st->hw_fam_lock_pclk != 0 && hw_lock_drift <= ARB_HW_DRIFT_HZ;

    if (!effective_blind && vic_is_hd(in->hw_vic)) {
        st->hd_det_vic = in->hw_vic;
        st->hd_det_enc = in->enc_hd_vic;
    }
    c->blind = blind;
    c->effective_blind = effective_blind;
    c->hw_reading_current = hw_reading_current;
    c->sd_clock_confirmed = sd_clock_confirmed;
    c->region = region;
    c->pal50_dbnc = pal50_dbnc;
    // Focus's too: L5 put a stale VIC 2's 480 lines on 640x576 (0x05) for ~560 ms before L6 got
    // there, and without the EEPROM region a VIC 1 first.
    c->narrow_576 = cx_576_narrow || xcal_576_narrow || focus_576_narrow;
    c->hw_narrow_576 = hw_narrow_576;
    c->lines = lines;
    c->bios_lines = bios_lines;
    c->hw_ruled_out = hw_ruled_out;
}

// Level 1: the confirmed BIOS mode.
static bool arb_level_bios(const arb_snapshot_t *in, arb_state_t *st, const arb_ctx_t *c,
                    vic_decision_t *out) {
    // ================= Level 1: BIOS =================
    // A packet still being applied needs nothing here: its burst holds the mode-change state
    // open, which rolls back every standalone apply until the new mode is confirmed.
    uint8_t bidx = 0xFF;
    if (!in->bios_confirmed) {
        st->bios_stale_since = 0;
    }
    if (in->bios_confirmed && in->confirmed_mode_index != 0xFF) {
        // A confirmed mode stays authoritative between packets unless the live signal has
        // moved to another family with SMBus silent -- e.g. a 720p BIOS screen handed to a
        // 640 standalone context, which otherwise pinned VIC 4 ("Unsupported" on the TV).
        // Never for BIOS-authoritative rows; needs a settled clock and ARB_BIOS_STALE_MS so
        // a mid-switch misread can't hand off. Stale if:
        //  (1) the detector reads a different family on the current raster -- one the line-count
        //      evidence doesn't rule out (see arb_assess(): an HD reading on an SD clock, a
        //      non-576 one at a Conexant narrow-576 clock, a stray 17 on Focus PAL-60 0x02 at
        //      29.455 MHz that released the BIOS 16:9 VIC 3 to L6's 4:3);
        //  (2) the line-count evidence rules the mode's own line count out: an HD mode on an
        //      SD clock (a 640 signal the detector reads as blind), a 576 mode in the Path A
        //      window (the PAL-I 480-line hybrid under a stale 576 confirm, mirrors L3);
        //  (3) the debounced width signal contradicts the mode's 480-line width. (1) can't
        //      see 640 <-> 720 because arb_family_agrees() merges those families.
        mode_family_t cf = xbox_mode_family(in->confirmed_mode_index, in->encoder_id);
        bool cf_is_sd_720 = (cf == FAM_SD_720x480);
        bool cf_is_sd_640 = (cf == FAM_SD_640);
        // Not a width held over from earlier content (width_stale_held: Focus's width went
        // stale in the SDTV band and the last verdict was kept). A stale "wide" from prior
        // content dropped confirmed 0x0F to L6 every time. (0x0F is excluded from staleness
        // altogether, as BIOS-authoritative: even a fresh width reading is wrong for it.)
        bool width_contradicts_bios = in->width_confirmed && !in->width_stale_held &&
            !in->width_predates_bios &&
            ((cf_is_sd_720 && !in->width_wide) || (cf_is_sd_640 && in->width_wide));
        bool stale = !arb_is_bios_authoritative(in->confirmed_mode_index) && in->pclk_stable &&
                     ((!c->blind && !c->hw_ruled_out && c->hw_reading_current &&
                       arb_family_of_vic(in->hw_vic) != FAM_NONE &&
                       !arb_family_agrees(in->hw_vic, arb_vic_for(cf, false))) ||
                      (arb_lines_of(cf) & ~c->bios_lines) ||
                      width_contradicts_bios);
        if (!stale) {
            st->bios_stale_since = 0;
            bidx = in->confirmed_mode_index;
        } else if (st->bios_stale_since == 0) {
            st->bios_stale_since = in->now_ms;
            bidx = in->confirmed_mode_index;                   // hold until debounced
        } else if ((in->now_ms - st->bios_stale_since) < ARB_BIOS_STALE_MS) {
            bidx = in->confirmed_mode_index;                   // still within the hold
        }
        // else: stale and held long enough -> leave bidx 0xFF, fall through to L3/L4
    }
    if (bidx != 0xFF) {
        mode_family_t fam = xbox_mode_family(bidx, in->encoder_id);
        if (fam != FAM_NONE) {
            bool ws, ana;
            // The packet's widescreen bit is the BIOS aspect hint. (0x02/0x06/0x08/0x11
            // are 720-wide in either aspect, so width says nothing about aspect.)
            arb_resolve_aspect(fam, in, in->bios_widescreen ? 1 : 0, &ws, &ana);
            // 0x05/0x12 are the 640x576 rows: family 720x576 for the checks above, but
            // the apply uses VIC 21/22, not 17/18. Only the logged/telemetry VIC sees this
            // -- an ARB_SRC_BIOS decision is never applied by standalone.
            uint8_t target_vic = (fam == FAM_SD_720x576 &&
                                  (xbox_mode_flags(bidx, in->encoder_id) & MODE_640))
                ? (ws ? VIC_22_576i_50_2xClk_16_9 : VIC_21_576i_50_2xClk_4_3)
                : arb_vic_for(fam, ws);
            return arb_done(out, arb_pick(ARB_SRC_BIOS, target_vic, ws, ana, in));
        }
    }
    return false;
}

// Level 3: Path A, the Xcalibur PAL-I 480-line hybrid raster.
static bool arb_level_path_a(const arb_snapshot_t *in, arb_state_t *st, const arb_ctx_t *c,
                    vic_decision_t *out) {
    // ================= Level 3: Path A (PAL-region hybrid raster) =================
    // Xcalibur, not on a known-NTSC console: a stable clock in [22.5, 23.95) MHz is the PAL-I
    // 480-line hybrid (23.4 MHz, 50 Hz), which the detector reads unreliably; Path A applies
    // the 720x480 family with its own HS delay. Not Focus (its 640-wide 50 Hz content shares
    // the window and its width signal resolves it in L6) and not Conexant (its PAL clocks,
    // 24.9 and 27.6 MHz, never reach the window). Steps aside when the width is positively
    // confirmed narrow -- Xcalibur 0x10 is 640-wide at the same 23.4 MHz.
    if (!in->bios_confirmed && c->region != ARB_REGION_NTSC &&
        in->encoder_id == ENCODER_XCALIBUR &&
        !(in->width_confirmed && !in->width_wide)) {
        // Admits a blind detector, or any reading at all once the clock is in the window: the
        // detector is non-deterministic here (VIC 1, VIC 2) and 0x3E can stick on a stale VIC
        // (e.g. 18 from a prior 576p50 mode), and nothing else on a PAL50 console runs in
        // [22.5, 23.95) MHz. Deliberately not gated on pclk_stable -- while blind every pick
        // goes through L6, whose apply re-disturbs the clock before it can settle (deadlock);
        // the ARB_PATH_A_HOLD_MS hold is the anti-flap protection.
        const bool pclk_in_path_a_win =
            in->pclk_hz >= ARB_PATH_A_PCLK_LOW_HZ && in->pclk_hz < ARB_PATH_A_PCLK_HIGH_HZ;
        const bool path_a_hw_ok = pclk_in_path_a_win || c->blind;
        // Active VICs Path A may replace: the 480-line SD VICs always; a 576 VIC only with a
        // stable clock in the window (a stale 576 VIC on the hybrid, e.g. after entering from
        // a 576 dashboard mode -- otherwise a deadlock only an MCU reset broke).
        const bool active_is_path_a_leavable = (in->active_vic == VIC_01_VGA_640x480_4_3 ||
                                       in->active_vic == VIC_02_480p_60__4_3 ||
                                       in->active_vic == VIC_03_480p_60_16_9 ||
                                       ((vic_is_576p(in->active_vic) || vic_is_576i(in->active_vic)) &&
                                        pclk_in_path_a_win && in->pclk_stable));
        // Once latched, Path A holds through clock drops and blips (including its own TMDS
        // cycle) and only un-latches on a clock above the window.
        // The target follows the resolved aspect (VIC 2 or 3), matching what the apply
        // produces -- a fixed VIC 3 never matched a 4:3 apply and re-cycled every ~800 ms. The
        // apply routes this VIC 2 through set_video_mode_vic_ex(XBOX_VIDEO_480p_720) for Path
        // A's HS delay (apply_standalone_by_vic()).
        bool pa_ws, pa_ana;
        arb_resolve_aspect(FAM_SD_720x480, in, -1, &pa_ws, &pa_ana);
        uint8_t pa_expected_vic = pa_ws ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3;
        if (in->pclk_hz >= ARB_PATH_A_PCLK_HIGH_HZ) {
            st->path_a_since = 0;
            st->path_a_latched = false;
        } else if (st->path_a_latched && active_is_path_a_leavable) {
            return arb_done(out, arb_pick(ARB_SRC_PATH_A, pa_expected_vic, pa_ws, pa_ana, in));
        } else if (path_a_hw_ok && active_is_path_a_leavable && !in->tmds_cycle_busy &&
                   in->pclk_hz > 0) {
            if (held_for(&st->path_a_since, in->now_ms, ARB_PATH_A_HOLD_MS)) {
                st->path_a_latched = true;
                return arb_done(out, arb_pick(ARB_SRC_PATH_A, pa_expected_vic, pa_ws, pa_ana, in));
            }
            st->verdict_pending = true;
        } else if (!path_a_hw_ok) {
            st->path_a_since = 0;
        }
    }

    return false;
}

// Level 4: the line detector's VIC.
static bool arb_level_hw_vic(const arb_snapshot_t *in, arb_state_t *st, const arb_ctx_t *c,
                    vic_decision_t *out) {
    // ================= Level 4: Hardware VIC =================
    // Acts on the detector's reading unless it is untrusted (effective_blind) or an HD reading
    // on a confirmed SD clock.
    // Settle gate first: an SD reading from before the clock moved describes the previous
    // raster while the new clock settles -- acting on it applied the old family, then the
    // real one (two blackouts). Hold until the clock settles. No clock at all (e.g. after an
    // HPD reconnect) holds too; a forced re-push restores the mode. HD readings are exempt.
    if (!c->blind && !c->hw_reading_current && st->hw_fam_lock_pclk != 0 &&
        !in->pclk_stable && !in->first_lock && arb_vic_is_sd(in->hw_vic)) {
        st->verdict_pending = true;
        return arb_done(out, arb_hold());
    }
    if (!c->effective_blind) {
        // Narrow 576 (see hw_narrow_576): straight to the VIC 21/22 placeholder, ahead of the
        // agreement logic below, which can't express "576 lines, but narrow". arb_pick() is a
        // no-op if VIC 21/22 is already active. A width left from the previous mode can't get
        // out: the mode-change state (xbox_video_standalone.c) rolls back every apply until
        // the clock settles.
        if (c->hw_narrow_576) {
            return arb_done(out, arb_pick_576i(ARB_SRC_HW_VIC, in));
        }

        mode_family_t fam = arb_family_of_vic(in->hw_vic);

        // PAL50: the detector can't tell the 480-line hybrid from 576p50 (both 720-wide near
        // 27 MHz), so a settled PAL50 clock verdict overrides its SD family. Not on Xcalibur
        // before its registers give a mode_index: there 720x480 and 720x576 share 27.000 MHz, the
        // clock-only verdict can't tell them apart, and the override flapped VIC 3 <-> 18.
        bool pal50_override = false;
        if (c->region == ARB_REGION_PAL50 && c->pal50_dbnc != VIC_00_VIC_Unavailable &&
            !(in->encoder_id == ENCODER_XCALIBUR && in->xcal_mode_index == 0) &&
            (fam == FAM_SD_640 || fam == FAM_SD_720x480 || fam == FAM_SD_720x576)) {
            fam = arb_family_of_vic(c->pal50_dbnc);
            pal50_override = true;
        }
        // First lock: active_vic is whatever init left, not a real mode, so adopt the family
        // at once, without the debounce. Needs a clock: 0x3E re-acquires faster than the ACR,
        // and a reading with pclk still 0 can be wrong (one wrong-VIC frame); pclk 0 takes the
        // debounced path below instead.
        if (in->first_lock && in->pclk_hz > 0) {
            st->hw_incompat_since = 0;
            return arb_done(out, arb_pick_family(ARB_SRC_HW_VIC, fam, in));
        }
        // Agreement: the resolved family against the applied one (arb_family_agrees() when PAL50
        // didn't override, for its 640/720x480 merge).
        mode_family_t af = arb_family_of_vic(in->active_vic);
        bool agree = pal50_override ? (af == fam)
                                    : arb_family_agrees(in->hw_vic, in->active_vic);
        // 576i and 576p share a family but are different scan formats: a mismatch either way
        // is disagreement.
        if ((vic_is_576i(in->active_vic) && vic_is_576p(in->hw_vic)) ||
            (vic_is_576p(in->active_vic) && vic_is_576i(in->hw_vic))) {
            agree = false;
        }
        // Same family, different aspect: the detector can't show it (VIC 2/3 and 17/18 are
        // electrically identical and just echo our last write), so check the resolved aspect
        // against the applied VIC. Only for those two pairs -- 640 has no 16:9 VIC, and a
        // blanket check would break arb_family_agrees()' 640/720 noise guard.
        // resolve_fam is what gets applied on a disagreement: the detector's family, except
        // in the VIC 1 branch below.
        mode_family_t resolve_fam = fam;
        if (af == fam && (fam == FAM_SD_720x480 || fam == FAM_SD_720x576)) {
            bool ws, ana;
            arb_resolve_aspect(fam, in, -1, &ws, &ana);
            if (vic_is_16_9(in->active_vic) != ws) {
                agree = false;
            }
        } else if (af == FAM_SD_640 && (fam == FAM_SD_640 || fam == FAM_SD_720x480)) {
            // VIC 1 active and the detector reads 480-line SD (VIC 1, or VIC 2 from wide-
            // shaped noise): check the anamorphic flag, and re-aspect VIC 1 in place on a
            // mismatch rather than promoting to 720. Not when the width is freshly confirmed
            // wide: that is a real 720 raster, and re-aspecting VIC 1 first cost a second
            // cycle when L5 applied VIC 2/3 moments later.
            const bool width_says_720 = in->width_confirmed && in->width_wide && !in->width_stale_held;
            bool ws, ana;
            arb_resolve_aspect(FAM_SD_640, in, -1, &ws, &ana);
            if (!width_says_720 && ana != in->active_is_anamorphic) {
                agree = false;
                resolve_fam = FAM_SD_640;
            }
        }
        if (agree) {
            st->hw_incompat_since = 0;
        } else {
            if (held_for(&st->hw_incompat_since, in->now_ms, ARB_HW_DEBOUNCE_MS)) {
                st->hw_incompat_since = 0;
                return arb_done(out, arb_pick_family(ARB_SRC_HW_VIC, resolve_fam, in));
            }
            st->verdict_pending = true;
        }
    } else {
        st->hw_incompat_since = 0;
    }

    return false;
}

// Level 5: the width signal against the applied 480-line VIC.
static bool arb_level_width(const arb_snapshot_t *in, arb_state_t *st, const arb_ctx_t *c,
                    vic_decision_t *out) {
    // ================= Level 5: Width re-check (SD narrow<->wide) =================
    // The width signal tells 640 from 720 geometry, not aspect, so this compares geometry to
    // geometry (VIC 2 and 3 are both 720-wide) and the target takes the resolved aspect --
    // targeting VIC 3 under a user 4:3 setting re-applied forever.
    // VIC 3 is watched only outside the Path A window on Xcalibur/Conexant, so this doesn't
    // fight L3 there. Below it nothing else did: a user 16:9 VIC 3 held on Xcalibur PAL
    // 640x480 (0x03, 20.8 MHz) for good. Focus has no Path A and a reliable width
    // signal in that band, so it always watches -- otherwise a Focus VIC 3 whose raster
    // narrowed to 640 at ~22.68 MHz had no way back to VIC 1.
    const bool watch = (in->active_vic == VIC_01_VGA_640x480_4_3 ||
                        in->active_vic == VIC_02_480p_60__4_3 ||
                        (in->active_vic == VIC_03_480p_60_16_9 &&
                         (in->encoder_id == ENCODER_FOCUS ||
                          in->pclk_hz < ARB_PATH_A_PCLK_LOW_HZ ||
                          in->pclk_hz >= ARB_PATH_A_PCLK_HIGH_HZ)));
    // Not on known 640x576 (Conexant's narrow-576 clocks, Xcalibur's mode index): this level
    // only knows the 480-line families, and would demote such content to VIC 1 (wrong
    // family) -- Xcalibur 0x10 -> 0x05 put out 640x480 before L6 got to VIC 22. L6 handles it.
    if (watch && in->pclk_stable && in->width_confirmed &&
        in->pclk_hz > 0 && in->pclk_hz < ARB_HD_PCLK_MIN_HZ && !c->narrow_576) {
        // 720-wide raster = FAM_SD_720x480 (VIC 2 AND VIC 3); 640 = FAM_SD_640.
        const bool active_geom_wide =
            arb_family_of_vic(in->active_vic) == FAM_SD_720x480;
        if (in->width_wide != active_geom_wide) {
            if (held_for(&st->width_mismatch_since, in->now_ms, ARB_WIDTH_HOLD_MS)) {
                st->width_mismatch_since = 0;
                // The family must match the target width: going to 640 needs FAM_SD_640
                // so a 16:9 want becomes anamorphic (FAM_SD_720x480 here once turned a
                // correct 640 anamorphic picture into plain 4:3). Wide where only 576 lines run
                // is 720x576 (Conexant's 576p band: VIC 3 went out ahead of L4's VIC 18, seeds
                // 2654, 2703).
                return arb_done(out, arb_pick_family(ARB_SRC_WIDTH, !in->width_wide ? FAM_SD_640 :
                                       c->lines == ARB_LINES_576 ? FAM_SD_720x576 : FAM_SD_720x480, in));
            }
            st->verdict_pending = true;
        } else {
            st->width_mismatch_since = 0;
        }
    } else {
        st->width_mismatch_since = 0;
    }

    return false;
}

// Level 5b: the width signal against an applied narrow-576 VIC.
static bool arb_level_width_576(const arb_snapshot_t *in, const arb_ctx_t *c, vic_decision_t *out) {
    // ============= Level 5b: Width re-check (SD 576-line narrow<->wide) =============
    // The 576 counterpart, one direction only: hw_narrow_576 (L4) already moves a stuck VIC
    // 17/18 to 21/22 whenever the width reads narrow. Nothing moved back: VIC 21/22 and a
    // 576-family reading "agree" on family, so confirmed-wide content held on VIC 21 (seen on
    // Conexant 0x06). No debounce of its own -- width_wide is already debounced upstream.
    // A 576 VIC up while the line-count evidence rules 576 out moves to the width's 480-line
    // family: a frozen Xcalibur 0x05/0x06/0x12 -> 0x02/0x0F keeps 27.000 MHz and the detector is
    // blind there -- VIC 17/18/21/22 stayed for good; a wide 480-line raster can land on VIC 21/22 through hw_narrow_576's
    // family-only match (Conexant 0x04/0x10, ~27.56 MHz), or follow a 640x576 apply (Focus
    // PAL-60 720x480, 29.455 MHz).
    if (in->width_confirmed && in->pclk_stable && !(c->lines & ARB_LINES_576) &&
        arb_family_of_vic(in->active_vic) == FAM_SD_720x576) {
        return arb_done(out, arb_pick_family(ARB_SRC_WIDTH, in->width_wide ? FAM_SD_720x480 : FAM_SD_640, in));
    }
    if (in->width_confirmed && in->width_wide && vic_is_576i(in->active_vic)) {
        // The width flips with the raster, ahead of the clock: hold for a settled one to test
        // the line count against (a Focus PAL-60 720x480 went out as VIC 17 at pclk 0).
        if (!in->pclk_stable) return arb_done(out, arb_hold());
        return arb_done(out, arb_pick_family(ARB_SRC_WIDTH, FAM_SD_720x576, in));
    }

    return false;
}

// Level 6: the blind fallback -- and hold when nothing above decided.
static vic_decision_t arb_level_blind(const arb_snapshot_t *in, arb_state_t *st, const arb_ctx_t *c) {
    // ================= Level 6: Blind fallback =================
    // Runs on effective_blind, so an untrusted reading gets the same region/width/clock
    // resolution as a truly blind one.
    if (c->effective_blind) {
        if (held_for(&st->blind_since, in->now_ms, ARB_BLIND_TIMEOUT_MS)) {
            // Hold rather than fall back to SD while the signal is really HD: a stable HD
            // clock (720p <-> 1080i share 74.25 MHz and go briefly blind), or an HD mode with
            // no confirmed drop to an SD clock yet (e.g. after a relock re-arms pclk_stable).
            // Otherwise the TV chased 640/720x480 for ~1.5 s before L4 recovered VIC 4/5.
            bool clock_says_hd = in->pclk_stable && in->pclk_hz >= ARB_HD_PCLK_MIN_HZ;
            bool active_is_hd = vic_is_hd(in->active_vic);
            mode_family_t af = arb_family_of_vic(in->active_vic);
            bool active_is_480_sd = (af == FAM_SD_640 || af == FAM_SD_720x480);
            // 576p only (not 576i): hold_576 protects a settled 576p picture from a detector
            // blink; including VIC 21/22 kept a stale 576i active even after the verdict
            // settled on 576p.
            bool active_is_576p = vic_is_576p(in->active_vic);
            // Holds on the current SD mode while blind:
            //  - hold_480: a settled 480-line SD mode on a steady SD clock, unless the
            //    debounced PAL50 verdict has moved to a 576 VIC;
            //  - hold_576: a settled 576p50, unless the verdict has moved to a 480-family VIC.
            // Both read the debounced verdict, so a release coincides with the fallback being
            // able to act on it. Neither holds in the Path A window (that is the PAL-I 480-line
            // hybrid, which L3/the fallback route); the ~20.8 MHz hybrid below the window is
            // still held -- there the blindness really is detector noise.
            bool clk_in_bg_hybrid_win = in->pclk_stable && !in->tmds_cycle_busy &&
                in->pclk_hz >= ARB_PATH_A_PCLK_LOW_HZ && in->pclk_hz < ARB_PATH_A_PCLK_HIGH_HZ;
            // hold_480 also releases on an aspect change while blind (otherwise the AVI
            // stays on the previous mode's aspect). VIC 1 compares the anamorphic flag.
            bool ws_480, ana_480;
            arb_resolve_aspect(af, in, -1, &ws_480, &ana_480);
            bool aspect_agrees_480 = (in->active_vic == VIC_01_VGA_640x480_4_3)
                ? (ana_480 == in->active_is_anamorphic)
                : (vic_is_16_9(in->active_vic) == ws_480);
            // Only while the PAL50 classifier has a verdict for this clock at all -- otherwise
            // none can arrive, and every hold ended in the ARB_HOLD_STUCK_TIMEOUT_MS escape on
            // an already-correct mode. An escape now means a verdict that keeps changing.
            const bool pal50_classifiable = st->pal50_sd_cand != VIC_00_VIC_Unavailable;
            bool hold_480 = pal50_classifiable &&
                            active_is_480_sd && c->sd_clock_confirmed && !clk_in_bg_hybrid_win &&
                            aspect_agrees_480 &&
                            c->pal50_dbnc != VIC_18_576p_50_16_9 &&
                            c->pal50_dbnc != VIC_21_576i_50_2xClk_4_3 &&
                            c->pal50_dbnc != VIC_22_576i_50_2xClk_16_9;
            // Conexant: hold_576 only near its own 576p clock. Its other PAL clocks also read
            // "576p" under the Focus-calibrated split, so the verdict never released the hold
            // (a 67 s lockup). Our own TMDS cycle (busy, or a clock of 0) is not evidence
            // against 576p, so the hold stays plausible through it.
            bool cx_pal576_plausible = in->encoder_id != ENCODER_CONEXANT ||
                                       in->tmds_cycle_busy || in->pclk_hz == 0 ||
                                       in->pclk_hz >= ARB_CONEXANT_PAL576_PCLK_MIN_HZ;
            // hold_576 also releases on an aspect change within 576p while blind, and on a
            // confirmed narrow width: 576p is 720-wide, and a 640x576 mode can share its clock
            // (Focus 0x06 -> 0x12, both 27.000 MHz, held 720x576 for good).
            bool ws_576, ana_576;
            arb_resolve_aspect(FAM_SD_720x576, in, -1, &ws_576, &ana_576);
            bool aspect_agrees_576 = (vic_is_16_9(in->active_vic) == ws_576);
            bool hold_576 = pal50_classifiable &&
                            c->region == ARB_REGION_PAL50 && active_is_576p && !clk_in_bg_hybrid_win &&
                            cx_pal576_plausible && aspect_agrees_576 &&
                            !(in->width_confirmed && !in->width_wide) &&
                            c->pal50_dbnc != VIC_03_480p_60_16_9 &&
                            c->pal50_dbnc != VIC_01_VGA_640x480_4_3;
            // The PAL50 verdict can stay VIC 0 forever for a clock outside its profiled bands.
            // Bound a hold that is waiting on an unresolved verdict to ARB_HOLD_STUCK_TIMEOUT_MS,
            // then fall through to the fallback (a hold on a settled verdict is fine and stays).
            bool holding_on_pal50 = (hold_480 || hold_576) && c->pal50_dbnc == VIC_00_VIC_Unavailable;
            if (holding_on_pal50) {
                st->verdict_pending = true;   // the verdict may yet move the mode
                if (held_for(&st->hold_stuck_since, in->now_ms, ARB_HOLD_STUCK_TIMEOUT_MS)) {
                    hold_480 = false;
                    hold_576 = false;
                    st->hold_stuck_escaped = true;
                }
            } else {
                st->hold_stuck_since = 0;
                st->hold_stuck_escaped = false;
            }
            // A settled HD clock and the encoder's own registers name the HD mode: that one.
            // The clock can't tell 720p from 1080i, and holding put nothing at all out on a
            // frozen boot into HD (seen on Focus).
            // Not over the detector's own reading of the active mode while the register still
            // holds what it held then: a sniffer that lost Focus's 0x92 write kept 720p's, and
            // a blind blink after the detector's 1080i put 720p out for good.
            if (clock_says_hd && in->enc_hd_vic && in->enc_hd_vic != in->active_vic &&
                !(st->hd_det_vic == in->active_vic && st->hd_det_enc == in->enc_hd_vic)) {
                return arb_pick(ARB_SRC_BLIND_FALLBACK, in->enc_hd_vic, true, false, in);
            }
            if (clock_says_hd || (active_is_hd && !c->sd_clock_confirmed) ||
                hold_480 || hold_576) {
                // Holding, the candidate isn't refreshed: left over from before (an aspect write
                // since), it kept stand_alone_settled() false for good, and a deferred clock-band
                // re-apply never went (0x0E's row stayed on 0x04).
                if (hold_480 || hold_576) st->blind_fb_cand_vic = 0;
                return arb_hold();
            }
            bool fb_ws, fb_ana;
            uint8_t fb_vic = arb_blind_fallback_vic(in, c->region, c->lines, &fb_ws, &fb_ana);
            // Latch: a new candidate must hold for ARB_BLIND_FALLBACK_HOLD_MS. It is frozen
            // during our own TMDS cycle (busy, clock 0) and until the clock is stable -- before
            // that arb_pal50_sd_vic() can't classify, and the crude guess got applied before
            // the real verdict (seen on Conexant PAL). Nothing is output before the
            // first commit of a blind episode.
            // Back to Auto since the latch committed (a defaults reset): its aspect was the old
            // setting's, and put the old 4:3 out a debounce ahead of Auto's 16:9. It
            // re-commits once the live guess has held.
            if (in->user_aspect == ARB_ASPECT_AUTO && st->blind_fb_aspect != ARB_ASPECT_AUTO) {
                st->blind_fb_valid = false;
            }
            if (!in->tmds_cycle_busy && in->pclk_hz > 0 && in->pclk_stable) {
                if (fb_vic != st->blind_fb_cand_vic || fb_ws != st->blind_fb_cand_ws ||
                    fb_ana != st->blind_fb_cand_ana) {
                    st->blind_fb_cand_vic = fb_vic;
                    st->blind_fb_cand_ws = fb_ws;
                    st->blind_fb_cand_ana = fb_ana;
                    st->blind_fb_cand_since = in->now_ms;
                } else if ((in->now_ms - st->blind_fb_cand_since) >= ARB_BLIND_FALLBACK_HOLD_MS) {
                    st->blind_fb_valid = true;
                    st->blind_fb_vic = fb_vic;
                    st->blind_fb_ws = fb_ws;
                    st->blind_fb_ana = fb_ana;
                    st->blind_fb_aspect = (uint8_t)in->user_aspect;
                }
            }
            // The live verdict already matches the screen: nothing to change. The latch only
            // debounces changes, and its older value must not undo one already made -- a
            // settings re-apply (scan, colour, wire VIC) puts a new aspect on before the latch
            // catches up, and applying the latch brought the old aspect back (new -> old -> new
            // on 640x480).
            if (fb_vic == in->active_vic && fb_ana == in->active_is_anamorphic) {
                return arb_hold();
            }
            if (!st->blind_fb_valid) {
                return arb_hold();
            }
            // The latch debounces the raster guess, and the aspect with it: the encoder's live
            // aspect moves mid mode set (Xcalibur 0x07). An explicit Aspect setting doesn't: it
            // goes on the latched raster at once, whatever the live guess (still unsettled
            // after a reconnect). The latched aspect put a pre-reconnect 4:3 back on 640x480
            // after a 16:9 write landed while the clock settled (Focus).
            fb_vic = st->blind_fb_vic;
            fb_ws = st->blind_fb_ws;
            fb_ana = st->blind_fb_ana;
            if (in->user_aspect != ARB_ASPECT_AUTO) {
                fb_vic = vic_pair_base(fb_vic);
                fb_ws = in->user_aspect == ARB_ASPECT_16_9;
                fb_ana = fb_ws && fb_vic == VIC_01_VGA_640x480_4_3;
                if (fb_ws && !fb_ana) fb_vic++;   // the pair's 16:9 half (3, 18, 22)
            }
            return arb_pick(ARB_SRC_BLIND_FALLBACK, fb_vic, fb_ws, fb_ana, in);
        }
    } else {
        arb_clear_blind_fallback(st);
    }

    return arb_hold();
}

// The ladder: the first level that decides wins.
static vic_decision_t decide_target_vic_impl(const arb_snapshot_t *in, arb_state_t *st) {
    arb_ctx_t c;
    arb_assess(in, st, &c);
    vic_decision_t d;
    st->verdict_pending = false;
    if (arb_level_bios(in, st, &c, &d)) return d;
    if (arb_level_path_a(in, st, &c, &d)) return d;
    if (arb_level_hw_vic(in, st, &c, &d)) return d;
    if (arb_level_width(in, st, &c, &d)) return d;
    if (arb_level_width_576(in, &c, &d)) return d;
    return arb_level_blind(in, st, &c);
}


bool arb_clock_contradicts_active_family(const arb_snapshot_t *in) {
    const mode_family_t active = arb_family_of_vic(in->active_vic);
    // Known PAL50 console: the PAL50 classifier (480 vs 576, narrow vs wide). Only there --
    // its bands are PAL-only.
    if (arb_region(in) == ARB_REGION_PAL50) {
        mode_family_t now = arb_family_of_vic(arb_pal50_sd_vic(in));
        return now != FAM_NONE && now != active;
    }
    // Elsewhere: a fresh, confirmed width that contradicts the active VIC's width -- e.g.
    // NTSC 640 (VIC 1) -> 720x480: Conexant's H_ACTIVE says wide ~0.15 s after the packet,
    // well before the drift check's stable-clock window.
    if (!in->width_confirmed || in->width_stale_held) {
        return false;
    }
    return (active == FAM_SD_640 && in->width_wide) ||
           (active == FAM_SD_720x480 && !in->width_wide);
}

// Public entry: decide_target_vic_impl(), and on an apply -- unless the blind fallback made it
// -- the end of any blind episode: its latch is cleared (L6 clears it itself).
vic_decision_t decide_target_vic(const arb_snapshot_t *in, arb_state_t *st) {
    vic_decision_t d = decide_target_vic_impl(in, st);
    if (d.apply && d.source != ARB_SRC_BLIND_FALLBACK) {
        arb_clear_blind_fallback(st);
    }
    return d;
}
