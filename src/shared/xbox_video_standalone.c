// SPDX-License-Identifier: GPL-3.0-only
// Standalone video: works out the mode from the signal when no BIOS packet names it. Feeds
// the arbiter (vic_arbiter.c) a snapshot each main-loop pass and applies what it decides.

#include <stdbool.h>
#include <stdint.h>

#include "xbox_video_standalone.h"
#include "xbox_video_timing.h"
#include "adv7511.h"
#include "adv7511_xbox.h"
#include "user_config.h"
#include "debug.h"
#include "vic_arbiter.h"
#include "xbox_mode_index.h"
#include "xbox_eeprom_video.h" // pure decode; xbox_eeprom_decode_widescreen() below
// This file is application-only: the encoder/EEPROM sniffs (smbus_probe.h), the BIOS path's
// packet state and the SMBus telemetry/freeze (xbox_video_bios.h, smbus_i2c.h).
#include "../application/smbus_probe.h"
#include "../application/smbus_i2c.h"
#include "../application/xbox_video_bios.h"


// 640 vs 720 from the pixel clock: narrow (640, ~24.5 MHz) vs wide (720, ~27.0 MHz). The two
// thresholds form a hysteresis band; a reading inside it isn't a sample either way, so noise
// near the split can't flip the verdict.
#define SD_WIDTH_THRESHOLD_WIDE_HZ   26500000UL
#define SD_WIDTH_THRESHOLD_NARROW_HZ 25500000UL

// One clock reading, no state: 1 = wide, 0 = narrow, -1 = inconclusive (in the band, or under
// 20 MHz -- e.g. a gated clock mid-cycle; real narrow NTSC is above that). The clock fallback
// of sd_width_debounce_update().
static int sd_width_classify_instant(uint32_t pclk_hz) {
    if (pclk_hz > SD_WIDTH_THRESHOLD_WIDE_HZ) {
        return 1;
    }
    if (pclk_hz < SD_WIDTH_THRESHOLD_NARROW_HZ && pclk_hz >= 20000000UL) {
        return 0;
    }
    return -1;
}

// The width debounce, advanced once per stand_alone_loop() pass by sd_width_debounce_update();
// everything in the loop reads the same result through sd_width_is_wide()/_is_confirmed().
static struct {
    uint8_t  candidate;       // 0xFF = none yet
    uint8_t  stable;          // last debounced verdict (0 narrow / 1 wide)
    uint32_t candidate_start;
    bool     confirmed;       // has `stable` ever held a genuinely debounced sample this boot
    uint8_t  last_sample;     // hysteresis memory -- persists across in-band reads
    uint8_t  source;          // which input the last sample came from (see
                               // sd_width_last_source())
    uint8_t  focus_iho;       // Focus IHO as of the last update if fresh, else 0
} s_sdw = { .candidate = 0xFF };

// Which input produced the last width sample, for the [ARB] line's wsrc= field: 0 pclk,
// 1 focus-ihw, 2 cx-hact, 3 focus-ihw-stale-held, 4 xcal-reg (host tools name them).
uint8_t sd_width_last_source(void) {
    return s_sdw.source;
}

// Focus IHW width, same 1/0 shape: the threshold sits between the 640 and ~702 readings.
#define FS_IHW_WIDE_THRESHOLD_PX 670u
static int focus_ihw_classify_instant(uint16_t ihw_px) {
    return (ihw_px >= FS_IHW_WIDE_THRESHOLD_PX) ? 1 : 0;
}

// Focus FS454 IHO (Input Horizontal Offset) width classification:
// The Xbox NV2A driver keeps IHW=720 and shifts IHO by ~40 pixels to place 640 active
// pixels centered within the 720 raster ((720-640)/2 = 40):
//   720-wide: PAL standard (0x04/0x06) = 103 (0x67); NTSC standard (0x02) = 143 (0x8F).
//   640-wide: PAL 1.1:1 PAR (0x10/0x12) = 115; PAL standard (0x03/0x05) = 146;
//             NTSC 1.1:1 PAR (0x0F) = 151; NTSC standard (0x01) = 182.
// Across all Focus SD modes, IHO 103 and 143 are 720-wide; all others are 640-wide.
static bool focus_iho_is_720_wide(uint8_t iho) {
    return (iho == 103 || iho == 143);
}
// An IHO describes the raster up only if its region fits the clock: the NTSC values run only at
// the ~29.455 MHz interlaced clock, the PAL ones below it (22.68 MHz, 27.000 for 576i). A mode
// set whose IHO the sniffer missed kept the other region's -- NTSC 0x0F's 151 put its 640 row
// on PAL 22.68 MHz, PAL 0x10's 115 its row on NTSC. A region
// check, not a drift baseline: IHO is written on the old clock (see sd_width_debounce_update()).
static bool focus_iho_fits_clock(uint8_t iho, uint32_t pclk_hz) {
    const bool ntsc = iho == 182 || iho == 143 || iho == 151;
    const bool pal = iho == 146 || iho == 103 || iho == 115;
    return !pclk_hz || (pclk_hz >= ARB_FOCUS_SDTV_PCLK_MIN_HZ ? !pal : !ntsc);
}

// Conexant runs nearly every SD mode on a clock of its own (measured from recorded SMBus bursts): 640 at
// 24.42/24.625/24.937 MHz (H_ACTIVE 648), 640 1.1:1 PAR at 26.874/27.125 (704), 480p at 27.000
// (0x2E=0xAD, no H_ACTIVE), 720 at 27.377/27.687 (728); 27.562 is 0x04's 728 and 0x10's 704. A
// latched H_ACTIVE or RASTER_SEL that doesn't fit the clock is left from a mode set whose writes
// the sniffer missed: the clock decides (0x02's 728 kept 720 on 0x0F, 480p's 0xAD kept the 480p
// row on 0x02). Within 60 kHz: the clocks repeat to 1 kHz, the
// nearest two are 125 kHz apart.
static bool cx_near(uint32_t pclk_hz, uint32_t khz) {
    const uint32_t f = khz * 1000u;
    return pclk_hz + 60000u >= f && pclk_hz <= f + 60000u;
}
static bool cx_hactive_fits_clock(uint16_t hactive_px, uint32_t pclk_hz) {
    if (!pclk_hz || pclk_hz >= ARB_HD_PCLK_MIN_HZ) return true;
    if (hactive_px == 648) return pclk_hz < 25200000UL;
    if (hactive_px == 704) return cx_near(pclk_hz, 26874) || cx_near(pclk_hz, 27125) || cx_near(pclk_hz, 27562);
    if (hactive_px == 728) return cx_near(pclk_hz, 27377) || cx_near(pclk_hz, 27562) || cx_near(pclk_hz, 27687);
    return true;
}
// RASTER_SEL's progressive (480p) is 27.000 MHz only; interlaced (0x00) never.
static bool cx_int_prog_fits_clock(bool interlaced, uint32_t pclk_hz) {
    return !pclk_hz || pclk_hz >= ARB_HD_PCLK_MIN_HZ || interlaced != cx_near(pclk_hz, 27000);
}
// Only an old value is judged against the clock: one written before the newest encoder burst
// began (mode_change_update()), or over CX_FRESH_WRITE_MS ago. A mode set writes them a few ms
// before the clock slews (a 728 -> 704 must commit at once);
// a mode set whose writes were missed leaves the previous one's. Either alone missed some: by
// age, a 0x02 RASTER_SEL passed onto a 0x08 set 0.8 s later; by burst, a set too soon after
// the last for a quiet gap counted the last one's as fresh for good. [0] H_ACTIVE, [1]
// RASTER_SEL: the write epoch last seen and when it
// changed (cx_note_writes()).
#define CX_FRESH_WRITE_MS 1000u
static uint32_t s_burst_at;   // when the newest burst began
static uint8_t s_cx_epoch[2];
static uint32_t s_cx_epoch_at[2];
static void cx_note_writes(void) {
    const uint8_t e[2] = { xbox_cx_get_hactive_epoch(), xbox_encoder_get_int_prog_epoch(ENCODER_CONEXANT) };
    for (int i = 0; i < 2; i++) {
        if (e[i] != s_cx_epoch[i]) {
            s_cx_epoch[i] = e[i];
            s_cx_epoch_at[i] = HAL_GetTick();
        }
    }
}
static bool cx_write_old(int i) {
    return s_cx_epoch_at[i] < s_burst_at || HAL_GetTick() - s_cx_epoch_at[i] >= CX_FRESH_WRITE_MS;
}
// The width the clock alone gives: -1 at 27.562 (0x04 / 0x10) and off every Conexant SD clock.
static int cx_clock_width(uint32_t pclk_hz) {
    if (pclk_hz >= 20000000UL && pclk_hz < 25200000UL) return 0;
    if (cx_near(pclk_hz, 26874) || cx_near(pclk_hz, 27125)) return 0;
    if (cx_near(pclk_hz, 27000) || cx_near(pclk_hz, 27377) || cx_near(pclk_hz, 27687)) return 1;
    return -1;
}

// Conexant width from H_ACTIVE (regs 0x78 + 0x86[6:4]) = active width + a fixed offset, no
// clock involved; RASTER_SEL (0x2E) breaks the one tie. Matched every Conexant snapshot:
// - 728: 720-wide (modes 0x02/0x04/0x06) -- always WIDE (1).
// - 648: 640-wide (modes 0x01/0x03/0x05) -- always NARROW (0).
// - 704: SDTV raster (0x2E=0x00) = 640-wide 1.1:1 PAR (modes 0x0F/0x10/0x12), NARROW;
//        progressive 480p (0x2E=0xAD) = 720-wide raster (modes 0x07/0x08/0x11), WIDE.
// raster_prog: 1 = 0x2E says progressive, 0 = SDTV, -1 = 0x2E not seen yet this boot.
// Deliberately not gated on s_sd_int_prog_stale: the encoder doesn't rewrite an unchanged
// 0x2E, so that drift check fires on narrow 648 -> narrow 704 switches (~2.5 MHz jumps), and
// the clock fallback reads those 704 clocks (26.9-27.6 MHz) as wide. The latched 0x2E value
// matched the real raster in all 50 Conexant snapshots, rewritten or not.
static int cx_hactive_classify_instant(uint16_t hactive_px, int raster_prog) {
    if (hactive_px == 728) return 1;   // 720-wide standard
    if (hactive_px == 648) return 0;   // 640-wide standard
    if (hactive_px == 704) return raster_prog;   // prog = 720 raster, SDTV = 640 PAR, -1 defer
    return -1;
}

// Focus: SDTV family vs progressive 480p family, from the clock -- a different axis from
// width (on Focus only the family moves the clock). Thresholds and rationale in vic_arbiter.h.
static int focus_sd_family_classify_instant(uint32_t pclk_hz) {
    if (pclk_hz >= ARB_FOCUS_SDTV_PCLK_MIN_HZ) return 1;
    if (pclk_hz <= ARB_FOCUS_PROGRESSIVE_PCLK_MAX_HZ && pclk_hz >= 20000000UL) return 0;
    return -1;
}

// Xcalibur width from the mode index its raster registers give (xbox_xcal_mode_index()).
// 0x07 is 640-wide here (~24.545 MHz, 800 clocks/line, in every capture) though 720-wide on
// Conexant/Focus: this encoder outputs it natively instead of scaling it. Conexant's
// 720-wide timing on it gives a 28.6 kHz line rate and a black screen.
// 0x05/0x12 (640x576) are narrow too -- the clock fallback reads their 27.000 MHz as wide.
// Any other known index is wide (720, or HD); 0 (no mode set seen yet) is -1.
// Xcalibur's mode index (from its raster registers, at the end of the burst) names its clock:
// every captured burst runs 0x03 at 20.8 MHz, 0x04 at 23.4, 0x01/0x07
// at 24.545 and 0x02/0x05/0x06/0x08/0x12 at 27.000. True when the clock is off the index's (in
// 100 kHz, offset by 200; HD and unknown indexes never are): the clock lagging a new index
// (xcal_clock_lags_index()), or, once a mode change has closed, an index left from a mode set
// the sniffer missed.
static bool xcal_clock_off_index(uint8_t idx, uint32_t pclk_hz) {
    static const uint8_t k_clk[0x13] = { [0x01] = 45, [0x02] = 70, [0x03] = 8, [0x04] = 34,
        [0x05] = 70, [0x06] = 70, [0x07] = 45, [0x08] = 70, [0x12] = 70 };
    if (idx >= sizeof k_clk || !k_clk[idx] || !pclk_hz) return false;
    const int d = (int)(pclk_hz / 100000u) - 200 - k_clk[idx];
    return d > 3 || d < -3;
}

// The Xcalibur index the arbiter and the width classifier go on: none (0) while it's off the
// clock -- its mode set's writes were missed and the clock has moved on (0x01's 640 sent at
// 27 MHz). A new index the clock hasn't caught up with yet reads 0
// too, but then a mode change is open (xcal_clock_lags_index() keeps the raw one) and nothing
// applies.
static uint8_t xcal_index_on_clock(uint32_t pclk_hz) {
    const uint8_t idx = xbox_xcal_mode_index();
    return xcal_clock_off_index(idx, pclk_hz) ? 0 : idx;
}

static int xcal_mode_index_classify_instant(uint8_t idx) {
    const uint8_t m = xbox_mode_flags(idx, ENCODER_XCALIBUR);
    if ((m & MODE_FAM_MASK) == FAM_NONE) return -1;
    return (m & MODE_640) ? 0 : 1;
}

// The VIC and aspect a re-push of `vic` puts out: the aspect the arbiter would give it now
// (sd_resolve_widescreen() is its Auto order), and for a 4:3/16:9 pair (2/3, 17/18, 21/22)
// the sibling that carries it. Re-pushed with the VIC's old aspect, the arbiter switched it
// right after: a user 4:3/16:9 put back to Auto by a defaults reset, or a mode set whose
// WSS changed the aspect with the clock.
static uint8_t sd_repush_vic(const xbox_encoder xb_encoder, uint8_t vic, bool *wide) {
    const bool pair = vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9 ||
                      vic_is_576p(vic) || vic_is_576i(vic);
    *wide = sd_resolve_widescreen(xb_encoder, pair && vic_is_16_9(vic));
    if (pair && *wide != vic_is_16_9(vic)) {
        vic = *wide ? vic + 1 : vic - 1;   // each pair is 4:3 then 16:9
    }
    return vic;
}

// Focus SDTV-vs-480p family debounce (50 ms, like s_sdw), advanced from
// sd_width_debounce_update(). Never updated for other encoders -- check the encoder before
// trusting sd_family_is_sdtv()/_is_confirmed().
static struct {
    uint8_t  candidate;
    uint8_t  stable;
    uint32_t candidate_start;
    bool     confirmed;
} s_sdfam = { .candidate = 0xFF };

// The latched INT_PROG value has no expiry (some titles write the register once), so it can
// outlive the raster it described. Set when the clock has moved far from where the value was
// last confirmed. Focus then falls back to s_sdfam, Conexant to its own clock bands
// (cx_sd_is_sdtv_family()); the width classifier ignores it (cx_hactive_classify_instant()).
// One flag serves both, since only one encoder is live per boot.
static bool s_sd_int_prog_stale = false;

// Staleness of a latched encoder register: a clock move this large from where its value was
// last confirmed means a raster it wasn't rewritten for (see sd_width_debounce_update()).
#define TRUTH_STALE_PCLK_DRIFT_HZ     200000UL    // Focus IHW
#define INT_PROG_STALE_PCLK_DRIFT_HZ  1000000UL   // Focus 0x92 / Conexant 0x2E interlace flag

// One width sample per main-loop pass, from the best signal the encoder offers: Focus IHO,
// then IHW; Conexant H_ACTIVE; Xcalibur's register mode index; otherwise the pixel clock.
void sd_width_debounce_update(const xbox_encoder xb_encoder, uint32_t pclk_hz) {
    // Focus: IHW, always known (the chip's power-on 720 until the console writes it; the clock
    // alone is unreliable here).
    const bool is_focus = (xb_encoder == ENCODER_FOCUS);
    const uint16_t fs_ihw = is_focus ? xbox_focus_get_ihw() : 0;
    // have_fs_ihw drops to false when the IHW value has gone stale (staleness guard below), or
    // reads 0: a progressive mode set since its last write (see xbox_focus_get_ihw()).
    bool have_fs_ihw = is_focus && fs_ihw;
    // Conexant: H_ACTIVE (+ 0x2E for 704) classifies every SD mode with no clock involved; the
    // clock can't -- 0x0F/0x10/0x12 (narrow) sit at ~27 MHz on the wide side of the
    // threshold. The clock is only the fallback until H_ACTIVE has been seen.
    uint16_t cx_hactive;
    bool have_cx_hactive = (xb_encoder == ENCODER_CONEXANT) && xbox_cx_get_hactive(&cx_hactive);
    // Staleness guard for the latched width signal (Focus IHW / Conexant H_ACTIVE; never both
    // in one boot, so one baseline serves). The sniff has no expiry, and the console doesn't
    // always rewrite it for a different raster: a stale value gives a confident wrong width
    // (pillarboxing). Baseline the clock when the value or its write epoch changes; a later
    // multi-MHz departure means a raster the value was never rewritten for. Drift-based, not
    // clock-stability-based: a settling blip with no mode change would otherwise discard a
    // correct value for the rest of the mode (content doesn't always re-send it).
    uint16_t truth_raw = 0;
    uint8_t  truth_epoch = 0;
    bool     have_truth = false;
    if (is_focus) {
        truth_raw = fs_ihw;
        truth_epoch = xbox_focus_get_ihw_epoch();
        have_truth = true;
    } else if (have_cx_hactive) {
        truth_raw = cx_hactive;
        truth_epoch = xbox_cx_get_hactive_epoch();
        have_truth = true;
    }
    static uint16_t s_truth_baseline_raw = 0xFFFFu;
    static uint32_t s_truth_baseline_pclk = 0;
    static uint8_t  s_truth_baseline_epoch = 0;
    // Re-baseline only on a stable clock: the value is written while the GPU PLL is still
    // slewing, and baselining the old clock then would make the settled new clock look like
    // drift for the rest of the mode. Until it settles, the epoch mismatch keeps this branch
    // (not the drift one) running.
    if (have_truth) {
        if (truth_raw != s_truth_baseline_raw || truth_epoch != s_truth_baseline_epoch ||
            s_truth_baseline_pclk == 0) {
            if (pclk_hz > 0 && adv7511_pixel_clock_stable()) {
                s_truth_baseline_raw = truth_raw;
                s_truth_baseline_epoch = truth_epoch;
                s_truth_baseline_pclk = pclk_hz;
            }
        } else if (pclk_hz > 0 && is_focus) {
            // The drift check is Focus-only. Conexant doesn't need it --
            // cx_hactive_classify_instant() re-derives from the current value every call --
            // and it would misfire there: H_ACTIVE is written a few ms before the clock slews,
            // and two modes can share a value (704 for 0x10 at 27.562 MHz and 0x12 at
            // 27.125 MHz), so a real clock change reads as staleness.
            uint32_t drift = (pclk_hz > s_truth_baseline_pclk) ? (pclk_hz - s_truth_baseline_pclk)
                                                               : (s_truth_baseline_pclk - pclk_hz);
            if (drift > TRUTH_STALE_PCLK_DRIFT_HZ) {
                have_fs_ihw = false;
            }
        }
    }
    int pclk_inst = sd_width_classify_instant(pclk_hz);
    // Conexant: a latched H_ACTIVE off the clock is stale; then the clock's own width.
    if (xb_encoder == ENCODER_CONEXANT) cx_note_writes();
    if (have_cx_hactive && cx_write_old(0) && !cx_hactive_fits_clock(cx_hactive, pclk_hz)) {
        have_cx_hactive = false;
        pclk_inst = cx_clock_width(pclk_hz);
    } else if (xb_encoder == ENCODER_CONEXANT && !have_cx_hactive && cx_clock_width(pclk_hz) >= 0) {
        // None written yet (the first mode set's missed): the clock's own width too -- the
        // generic threshold read 0x12's 27.125 MHz as wide.
        pclk_inst = cx_clock_width(pclk_hz);
    }
    bool cx_raster_int;
    int cx_raster_prog = xbox_encoder_get_int_prog(ENCODER_CONEXANT, &cx_raster_int) &&
                         (!cx_write_old(1) || cx_int_prog_fits_clock(cx_raster_int, pclk_hz))
                         ? (cx_raster_int ? 0 : 1) : -1;
    int cx_hactive_inst = have_cx_hactive ? cx_hactive_classify_instant(cx_hactive, cx_raster_prog) : -1;

    // In-band / near-zero readings aren't fresh samples -- hold last_sample.
    // H_ACTIVE only on an SD clock: it is never rewritten in HD, so a value left from the last
    // SD mode must not be read during an HD excursion.
    bool use_cx_hactive = (xb_encoder == ENCODER_CONEXANT) && have_cx_hactive &&
                          (cx_hactive_inst >= 0) && (pclk_hz < ARB_HD_PCLK_MIN_HZ);
    // Focus with a stale IHW (the clock moved away from where it was written):
    // "don't know" (hold the last sample) in the clock bands where the clock can't tell
    // 640 from 720 -- the SDTV band (~29.454 MHz, shared by narrow and wide) and below the
    // PAL50 floor. There the clock read the SDTV band as wide, contradicting a correct
    // 640-wide BIOS packet. In the progressive 480p band 640 (~25.175 MHz) and 720
    // (~27.0 MHz) separate cleanly, so the clock is used.
    bool focus_pclk_ambiguous = (xb_encoder == ENCODER_FOCUS) && !have_fs_ihw &&
                                (pclk_hz >= ARB_FOCUS_SDTV_PCLK_MIN_HZ ||
                                 (pclk_hz > 0 && pclk_hz < ARB_PAL50_SD_PCLK_LOW_HZ));
    // Focus: IHO is unambiguous width truth for interlaced SD; otherwise IHW. Every interlaced
    // SD mode set writes IHO and then VID_CNTL0 (0x92) with INT_PROG set; 480p and HD write
    // neither IHO nor INT_PROG (0x92 = 0x3E/0x2E), so a progressive INT_PROG means the IHO is
    // the last SD mode's -- 0x0F's narrow 151 read 720-wide 480p as 640 on Focus. Not IHW's
    // clock-drift guard: that baselines whatever stable clock is present when the write
    // lands, and judged every fresh IHO stale across PAL's 22.68 <-> 27.000 MHz mode switches.
    uint8_t fs_iho;
    bool fs_interlaced;
    bool have_fs_iho = (xb_encoder == ENCODER_FOCUS) && xbox_focus_get_iho(&fs_iho) &&
        (!xbox_encoder_get_int_prog(ENCODER_FOCUS, &fs_interlaced) || fs_interlaced) &&
        focus_iho_fits_clock(fs_iho, pclk_hz);
    int fs_width_inst = -1;
    if (have_fs_iho) {
        fs_width_inst = focus_iho_is_720_wide(fs_iho) ? 1 : 0;
    } else if (have_fs_ihw) {
        fs_width_inst = focus_ihw_classify_instant(fs_ihw);
    }
    bool have_focus_width = (have_fs_iho || have_fs_ihw);
    s_sdw.focus_iho = have_fs_iho ? fs_iho : 0;

    int xcal_width_inst = (xb_encoder == ENCODER_XCALIBUR) ?
        xcal_mode_index_classify_instant(xcal_index_on_clock(pclk_hz)) : -1;
    // Physics guard: a clock in NTSC's narrow band (~24.545 MHz, 0x07) can't be a wide raster,
    // whatever the index table says -- a table mistake then fails safe. Band-limited because
    // PAL 0x04 is genuinely wide at 23.400 MHz on Xcalibur.
    if (xb_encoder == ENCODER_XCALIBUR && xcal_width_inst == 1 &&
        pclk_hz >= 24000000UL && pclk_hz < SD_WIDTH_THRESHOLD_NARROW_HZ) {
        xcal_width_inst = 0;
    }

    // The sample and its source (sd_width_last_source()): 0 pclk, 1 focus, 2 cx, 3 stale-held, 4 xcal.
    int inst = pclk_inst;
    uint8_t source = 0;
    if (xb_encoder == ENCODER_FOCUS && have_focus_width) {
        inst = fs_width_inst; source = 1;
    } else if (focus_pclk_ambiguous) {
        inst = -1; source = 3;
    } else if (xb_encoder == ENCODER_XCALIBUR && xcal_width_inst >= 0) {
        inst = xcal_width_inst; source = 4;
    } else if (use_cx_hactive) {
        inst = cx_hactive_inst; source = 2;
    }
    uint8_t sample = (inst >= 0) ? (uint8_t)inst : s_sdw.last_sample;
    s_sdw.last_sample = sample;
    s_sdw.source = source;

    uint32_t now_tick = HAL_GetTick();
    if (use_cx_hactive) {
        // Conexant H_ACTIVE is a discrete register value the console writes, not a measured
        // one -- nothing to debounce. The console's SD mode set writes RASTER_SEL (0x2E) before
        // H_ACTIVE (0x78), so a new H_ACTIVE is classified against the current raster type.
        // Committed at once so any decision after the write sees this mode's width, not the
        // previous one's.
        s_sdw.candidate = sample;
        s_sdw.candidate_start = now_tick;
        s_sdw.stable = sample;
        s_sdw.confirmed = true;
    } else if (sample != s_sdw.candidate) {
        s_sdw.candidate = sample;
        s_sdw.candidate_start = now_tick;
    } else if ((now_tick - s_sdw.candidate_start) >= 50) {
        s_sdw.stable = sample;
        s_sdw.confirmed = true;
    }

    // Focus family (SDTV vs 480p) debounce.
    if (xb_encoder == ENCODER_FOCUS) {
        int fam_inst = focus_sd_family_classify_instant(pclk_hz);
        if (fam_inst >= 0) {
            if ((uint8_t)fam_inst != s_sdfam.candidate) {
                s_sdfam.candidate = (uint8_t)fam_inst;
                s_sdfam.candidate_start = now_tick;
            } else if ((now_tick - s_sdfam.candidate_start) >= 50) {
                s_sdfam.stable = (uint8_t)fam_inst;
                s_sdfam.confirmed = true;
            }
        }
    }

    // INT_PROG staleness (see s_sd_int_prog_stale): the same baseline-and-drift approach as
    // the width guard. Runs for whichever encoder has the register (Xcalibur has none).
    bool int_prog;
    if (xbox_encoder_get_int_prog(xb_encoder, &int_prog)) {
        // s_int_prog_baseline_pclk == 0 means "never baselined".
        static bool s_int_prog_baseline = false;
        static uint32_t s_int_prog_baseline_pclk = 0;
        static uint8_t s_int_prog_baseline_epoch = 0;
        // A new write epoch re-baselines too: two rasters can share the same value.
        uint8_t int_prog_epoch = xbox_encoder_get_int_prog_epoch(xb_encoder);
        if (s_int_prog_baseline_pclk == 0 || int_prog != s_int_prog_baseline ||
            int_prog_epoch != s_int_prog_baseline_epoch) {
            s_int_prog_baseline = int_prog;
            s_int_prog_baseline_epoch = int_prog_epoch;
            s_int_prog_baseline_pclk = pclk_hz;
            s_sd_int_prog_stale = false;
        } else if (pclk_hz > 0) {   // pclk_hz==0 (gate-cycle blip) mustn't look like drift
            uint32_t drift = (pclk_hz > s_int_prog_baseline_pclk)
                ? (pclk_hz - s_int_prog_baseline_pclk) : (s_int_prog_baseline_pclk - pclk_hz);
            s_sd_int_prog_stale = drift > INT_PROG_STALE_PCLK_DRIFT_HZ;
        }
    }
}

bool sd_width_is_wide(void)      { return s_sdw.stable == 1; }
bool sd_width_is_confirmed(void) { return s_sdw.confirmed; }
bool sd_width_is_ihw_stale_held(void) { return s_sdw.source == 3; }
bool sd_width_predates_bios(void) {
    return s_sdw.source == 4 ? xbox_xcal_mode_index_predates_packet() :
           s_sdw.source == 1 ? xbox_encoder_width_predates_packet(ENCODER_FOCUS) :
           s_sdw.source == 2 && xbox_encoder_width_predates_packet(ENCODER_CONEXANT);
}

static bool sd_family_is_sdtv(void)      { return s_sdfam.stable == 1; }
static bool sd_family_is_confirmed(void) { return s_sdfam.confirmed; }

// Focus SDTV-family verdict: the INT_PROG bit as the console set it when seen (and not stale),
// else the debounced clock family. False for other encoders -- callers check the encoder.
static bool focus_sd_is_sdtv_family(void) {
    bool int_prog;
    if (!s_sd_int_prog_stale && xbox_encoder_get_int_prog(ENCODER_FOCUS, &int_prog)) {
        return int_prog;
    }
    return sd_family_is_confirmed() && sd_family_is_sdtv();
}

// Conexant SDTV-family (480i) verdict. Only 720-wide NTSC needs it -- 0x02 (480i) vs 0x08
// (480p), both near 27 MHz; 480i uses profile 0 (HS delay 135) instead of 480p's profile 1
// (120). 640-wide and PAL 720-wide are never interlaced on Conexant, and 0x2E reads 0x00
// ("int-SD") for almost every SDTV mode, so it is only trusted for that one case. Trusting it
// elsewhere routed 640 content to the Focus-only synthetic rows and PAL 720 to mode 0x02.
static bool cx_sd_is_sdtv_family(uint8_t vic, uint32_t pclk_hz) {
    // 640-wide: never interlaced on Conexant.
    if (vic == VIC_01_VGA_640x480_4_3 || vic == VIC_00_VIC_Unavailable) {
        return false;
    }
    // PAL 720-wide (0x04 ~27.56 MHz, near 0x06 ~27.69 MHz): never interlaced.
    if (pclk_hz >= 27450000UL && pclk_hz <= 27800000UL) {
        return false;
    }
    bool is_int;
    if (!s_sd_int_prog_stale && xbox_encoder_get_int_prog(ENCODER_CONEXANT, &is_int) &&
        (!cx_write_old(1) || cx_int_prog_fits_clock(is_int, pclk_hz))) {
        return is_int;
    }
    // Stale or never seen: the clock. Both real 480i rates are unambiguous once PAL 720 is
    // excluded -- ~27.378 MHz (the HD mode-test app) and ~29.454 MHz (retail BT.601 480i). Staleness is
    // common here: 640 NTSC baselines 0x2E=0x00 at ~24.42 MHz, a later 480i at ~27.378 MHz
    // doesn't rewrite the same value, and the ~3 MHz jump marks it stale.
    if ((pclk_hz >= 27250000UL && pclk_hz < 27450000UL) ||
        (pclk_hz >= 28500000UL && pclk_hz <= 30500000UL)) {
        return true;
    }
    return false;
}

static void set_video_mode_vic_ex(const xbox_encoder xb_encoder, const uint8_t mode, const bool widescreen, const bool interlaced, const bool anamorphic, const uint32_t pclk_hz, XboxVideoTiming *dry);

// Single [AUTODETECT] line, shared by every set_video_mode_vic_ex() call: what is being
// applied, in terms that can't be mistaken for the wire VIC (that's the "Actual VIC Sent :"
// line right after, which can honestly be 0 for a non-CEA raster such as 640x480p at
// 24.545MHz). p/i is the scan put on HDMI (only 1080i is interlaced; every SD raster on the
// pixel bus is progressive). "timing VIC" is the CEA family the timing row was built from
// (geometry, CSC, AVI). sdtv marks the SDTV-family timing row (HS delay / clock band) -- SD
// only: the caller's flag is plain "interlaced" for 1080i.
static void log_autodetect(const XboxVideoTiming *t, bool wide, bool anamorphic, bool sdtv_row, uint32_t pclk_hz) {
    // Flags and the clock in kHz (0 = none); a host tool writes the text.
    debug_log(LOG_AUTODETECT,
              t->active_w, t->active_h, t->interlaced_offset != 0, wide, anamorphic, t->delay_hs,
              t->vic, sdtv_row && t->active_w < 1280,   // HD passes it as "interlaced"
              (unsigned)(pclk_hz / 1000u));
}

// HD (VIC 4/5, always 16:9) and PAL 576 (17/18 720-wide, 21/22 the 640-wide placeholder)
// applies; any other VIC is ignored. The aspect is the VIC's own (17/21 are 4:3, 18/22 16:9)
// -- re-resolving it here could contradict the arbiter's choice and re-apply forever.
static void apply_hd_or_pal576_vic(const xbox_encoder xb_encoder, uint8_t vic, uint32_t pclk_hz,
                                   XboxVideoTiming *dry) {
    uint8_t mode;
    bool wide = false, interlaced = false;
    switch (vic) {
        case VIC_04_720p_60_16_9:       mode = XBOX_VIDEO_720p;     wide = true; break;
        case VIC_05_1080i_60_16_9:      mode = XBOX_VIDEO_1080i;    wide = true; interlaced = true; break;
        case VIC_17_576p_50__4_3:       mode = XBOX_VIDEO_576p_720; break;
        case VIC_18_576p_50_16_9:       mode = XBOX_VIDEO_576p_720; wide = true; break;
        case VIC_21_576i_50_2xClk_4_3:  mode = XBOX_VIDEO_576i_640; interlaced = true; break;
        case VIC_22_576i_50_2xClk_16_9: mode = XBOX_VIDEO_576i_640; wide = true; interlaced = true; break;
        default: return;
    }
    set_video_mode_vic_ex(xb_encoder, mode, wide, interlaced, false, pclk_hz, dry);
}

// The one VIC -> standalone apply mapping, used by stand_alone_loop() and
// reapply_active_video_mode(). VIC 2 is trusted as 720-wide (the arbiter's width debounce
// decided it). mailbox_widescreen is the arbiter's aspect, carried through the mailbox -- not
// re-resolved here, so a WSS/EEPROM change during a deferred apply can't disagree with the
// VIC already chosen.
// The 480-line SD VICs (0-3): 720 geometry for VIC 2/3, else 640, where a 16:9 want is
// anamorphic. Path A's HS delay (Xcalibur VIC 2 in its window) comes from
// set_video_mode_vic_ex(). With `dry`, only resolves the timing into it.
static void sd480_by_vic(const xbox_encoder xb_encoder, uint8_t vic, uint32_t pclk_hz, bool widescreen,
                         XboxVideoTiming *dry) {
    // SDTV-family (interlaced-row) pick; Xcalibur has none.
    const bool interlaced = (xb_encoder == ENCODER_FOCUS) ? focus_sd_is_sdtv_family()
                          : (xb_encoder == ENCODER_CONEXANT) ? cx_sd_is_sdtv_family(vic, pclk_hz)
                          : false;
    const bool geom_wide = (vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9);
    set_video_mode_vic_ex(xb_encoder, geom_wide ? XBOX_VIDEO_480p_720 : XBOX_VIDEO_480p_640,
                          widescreen, interlaced, !geom_wide && widescreen, pclk_hz, dry);
}

// The Xcalibur register mode index behind the last standalone apply. Rows that share a VIC and
// a clock -- 0x05/0x12 (640x576), 0x02/0x08 (720x480, a line apart), 0x01/0x07 (640x480, a
// line apart) -- only that index tells apart (xbox_video_timing.c), and a frozen switch
// between two of them changes neither VIC nor clock.
static uint8_t s_xcal_apply_idx;
static uint8_t xcal_row_pair(uint8_t idx) {
    return idx == 0x05 || idx == 0x12 ? 0x05 : idx == 0x02 || idx == 0x08 ? 0x02 :
           idx == 0x01 || idx == 0x07 ? 0x01 : 0;
}

void apply_standalone_by_vic(const xbox_encoder xb_encoder, uint8_t vic,
                              uint32_t pclk_hz, bool mailbox_widescreen) {
    s_xcal_apply_idx = (xb_encoder == ENCODER_XCALIBUR) ? xbox_xcal_mode_index() : 0;
    if (vic == VIC_00_VIC_Unavailable || vic == VIC_01_VGA_640x480_4_3 ||
        vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9) {
        sd480_by_vic(xb_encoder, vic, pclk_hz, mailbox_widescreen, NULL);
    } else {
        // HD and PAL 576 (VIC 4/5, 17/18/21/22). VIC 21/22 must land here too, or a stable
        // 576i detector reading never gets applied and the 30 ms debounce re-fires forever.
        apply_hd_or_pal576_vic(xb_encoder, vic, pclk_hz, NULL);
    }
}

// The clock behind the last 480-line or 640x576 SD apply and its class (sd480_class()), for
// stand_alone_loop()'s clock-move reapply. One at a time: an apply of anything else clears it.
static uint32_t s_sd480_apply_pclk = 0;
static uint8_t  s_sd480_apply_class = 0;

// 1 = 640 (VIC 1), 2 = 720x480 (VIC 2/3), 3 = 640x576 (VIC 21/22: Conexant 0x05/0x12 differ
// by clock alone -- a reconnect's blind VIC 21 took 0x05's row onto 0x12), 0 =
// anything else.
static uint8_t sd480_class(uint8_t vic) {
    return (vic == VIC_01_VGA_640x480_4_3) ? 1 :
           (vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9) ? 2 :
           vic_is_576i(vic) ? 3 : 0;
}

static void note_sd480_apply(uint8_t cls, uint32_t pclk_hz) {
    // No clock (a decision right after a reconnect): keep the class's known one -- the apply
    // picks its timing row from it (the mailbox below), and the band check above re-applies
    // if the raster turns out to have moved.
    if (!pclk_hz && cls && cls == s_sd480_apply_class && s_sd480_apply_pclk) {
        return;
    }
    s_sd480_apply_class = cls;
    s_sd480_apply_pclk = cls ? pclk_hz : 0;
}

// A BIOS packet takes over: drop a standalone apply still queued in the mailbox.
void stand_alone_cancel_queued(adv7511 *encoder) {
    encoder->vic &= ADV7511_VIC_CHANGED_CLEAR;
}

static bool s_settled = true;
bool stand_alone_settled(void) {
    return s_settled;
}
static bool s_change_settled = true;   // no mode change open, its verdict grace past
bool stand_alone_change_settled(void) {
    return s_change_settled;
}
static bool s_owner_settled = true;    // see stand_alone_owner_settled()
bool stand_alone_owner_settled(void) {
    return s_owner_settled;
}
// A re-push asked for (an HPD reconnect, a clock-band or row re-apply, a relock) that had to
// wait: done once nothing is under way, unless an apply has taken its place since.
static bool s_repush_wanted;
static uint8_t s_repush_applies;
static void want_repush(void) {
    s_repush_wanted = true;
    s_repush_applies = adv7511_apply_count();
}

// The clock still the old mode's after an Xcalibur mode set (xcal_clock_off_index()): the ADV
// can go on reading it as stable past the burst -- 0x11 -> 0x0E and 0x12 -> 0x01 decided at the
// old 27 MHz as the frozen packet came in, and the clock-band re-apply cycled TMDS again ~450 ms
// later. HD has no entry: the line-count evidence splits it by clock.
static bool xcal_clock_lags_index(const arb_snapshot_t *snap) {
    return snap->encoder_id == ENCODER_XCALIBUR && xcal_clock_off_index(xbox_xcal_mode_index(), snap->pclk_hz);
}

// One mode change at a time. The console opens it -- an encoder write, or a frozen packet with
// its burst still to come -- and so does the signal coming back from a loss: an HPD reconnect
// (the chip re-measures the clock; the old raster's re-push went out 400 ms ahead of the new
// raster's verdict) or a PLL loss (stand_alone_note_signal_loss()). It closes once
// its data is all in -- the encoder quiet for ENC_BURST_QUIET_MS and the clock stable (on
// Xcalibur, at its new mode index's clock) -- or MODE_CHANGE_MAX_MS after it opened (after the
// burst, when a frozen packet came first). While it is open nothing applies (stand_alone_loop()).
// No apply opens, extends or closes it, so nothing it holds back can hold it open.
// What a decision mid-change got wrong: the previous mode set's verdict (a frozen 720p's VIC 4
// 17 ms into the next 640 burst); the new index on the old aspect (Xcalibur writes
// 0x07 last, ~70 registers after 0x56: every PAL sweep's 0x0617 went out 16:9, then 4:3); the
// old clock still reading stable past the burst (0x11 -> 0x0E at 27 MHz, then a second cycle).
// Measured by logging every mode change (537 of them across the three encoders): the longest
// write gap inside a burst was 61 ms on Focus, 57 Xcalibur, 51 Conexant; the shortest between
// two bursts 119 ms.
#define ENC_BURST_QUIET_MS 100
#define MODE_CHANGE_MAX_MS 1500
// Re-pushes wait this long past a change too: the arbiter's verdict on the new raster forms
// over its debounces once the data is in -- L6's blind fallback (ARB_BLIND_FALLBACK_HOLD_MS),
// Path A (ARB_PATH_A_HOLD_MS), the longest single one -- and shows nothing pending before it starts (a
// settings re-apply put 720x480 back out 550 ms ahead of L6's 640x576).
#define VERDICT_GRACE_MS ARB_PATH_A_HOLD_MS
// Still the longest if a debounce grows. Summed where one can start after the other: L6's
// candidate latch only once blind has timed out, or once its hold on an unresolved PAL50
// verdict ends -- 550 ms, the closest (at 500 ms grace a reconnect re-apply put 640x480 out
// 50 ms ahead of L6's 640x576) -- and
// the hw debounce after a drift marks hw stale.
_Static_assert(VERDICT_GRACE_MS >= ARB_BLIND_TIMEOUT_MS + ARB_BLIND_FALLBACK_HOLD_MS &&
               VERDICT_GRACE_MS >= ARB_PAL50_SD_HOLD_MS + ARB_BLIND_FALLBACK_HOLD_MS &&
               VERDICT_GRACE_MS >= ARB_WIDTH_HOLD_MS &&
               VERDICT_GRACE_MS >= ARB_HW_DRIFT_HOLD_MS + ARB_HW_DEBOUNCE_MS &&
               VERDICT_GRACE_MS >= ARB_BIOS_STALE_MS,
               "a verdict debounce outlasts VERDICT_GRACE_MS: re-pushes would go out ahead of it");
static bool s_change_open;
static bool s_signal_loss_noted;   // stand_alone_note_signal_loss()
void stand_alone_note_signal_loss(void) {
    s_signal_loss_noted = true;
    s_settled = s_change_settled = s_owner_settled = false;
}
static bool s_burst_began;   // this pass: the console's first input after a quiet gap
static uint32_t s_change_since, s_change_closed_at;
static bool __attribute__((noinline)) mode_change_update(const arb_snapshot_t *snap, bool reconnect) {
    static uint32_t s_input_at, s_seen_write_at;
    static uint8_t s_seen_frozen;
    static bool s_expect_burst, s_capped, s_burst_since_packet;
    const uint32_t now = snap->now_ms, write_at = smbus_encoder_write_at();
    const uint8_t frozen = smbus_frozen_packet_count();
    const bool signal_event = reconnect || s_signal_loss_noted;
    bool input = signal_event;
    s_signal_loss_noted = false;
    const bool wrote = write_at != s_seen_write_at;
    if (wrote) {
        s_seen_write_at = write_at;
        // The burst a frozen packet announced: the change proper starts with it (capped from
        // the packet, it ended 22 ms into a burst that came 1.48 s later).
        if (s_expect_burst) s_change_since = now;
        s_expect_burst = false;
        s_burst_since_packet = input = true;
    }
    if (frozen != s_seen_frozen) {
        s_seen_frozen = frozen;
        // After a burst it closes that one and brings nothing new -- however late (359 ms
        // after: a quiet-time test waited for a burst that never came and held L5's verdict
        // for 0.9 s). With none since the last packet, its burst is still to come
        // (a reconnect re-push went out between the two).
        if (!s_burst_since_packet) s_expect_burst = input = true;
        s_burst_since_packet = false;
    }
    s_burst_began = input && now - s_input_at >= ENC_BURST_QUIET_MS;
    if (s_burst_began) s_burst_at = now;
    // Each burst after a quiet gap is a mode set of its own, so the cap counts from the
    // newest: counted from the first of six back-to-back sets, it cut Focus's long 0x0F burst
    // short and L4 put the stale 1080i VIC 5 out with no clock.
    if (s_burst_began && s_change_open) s_change_since = now;
    if (input) {
        s_input_at = now;
        // The cap is for a console that never goes quiet, not a new signal: its flag clears on
        // 100 ms of quiet, which can't come while HPD is low (no standalone pass), so a
        // reconnect after one opened nothing and a settings re-apply put the old 640 out on
        // the new raster.
        if (signal_event) s_capped = false;
        if (!s_change_open && !s_capped) {
            s_change_open = true;
            s_change_since = now;
        }
    }
    if (s_change_open) {
        if (!s_expect_burst && now - s_input_at >= ENC_BURST_QUIET_MS && snap->pclk_stable &&
            !xcal_clock_lags_index(snap)) {
            s_change_open = false;
        } else if (now - s_change_since >= MODE_CHANGE_MAX_MS) {
            // A console that never goes quiet: no hold until it has.
            s_change_open = s_expect_burst = false;
            s_capped = true;
        }
        if (!s_change_open) s_change_closed_at = now;
    }
    if (s_capped && now - s_input_at >= ENC_BURST_QUIET_MS) s_capped = false;
    return s_change_open;
}

// One standalone pass: build the arbiter snapshot, decide, and apply through the encoder->vic
// mailbox. True if a mode was applied this call (the caller then un-gates output); false if it
// was a no-op or is still waiting (blind detector, a debounce).
//
// first_lock: the supervisor's settle wait after a PLL (re)lock has resolved -- the arbiter
//   adopts the detector's reading at once (L4, no debounce), and an unchanged mode is
//   re-applied (the loss gated output).
// force_reapply: re-push the current mode even if its VIC is unchanged (HPD reconnect /
//   encoder self-latch reset AVI/CSC/packet state that a bare "no change" verdict skips). A
//   mode change of its own too: the chip re-measures the clock.
bool stand_alone_loop(adv7511 * encoder, const xbox_encoder xb_encoder, uint8_t current_hw_vic, uint8_t active_vic, uint32_t pclk_hz, bool first_lock, bool force_reapply) {
    // Advance the width debounce first, every pass -- including while blind (pclk 0 just holds
    // the last sample), since the width re-check runs while blind.
    sd_width_debounce_update(xb_encoder, pclk_hz);

    // === the standalone VIC decision -- ONE decide_target_vic() call ===
    // ARB_SRC_BIOS decisions are applied by bios_reconcile_pending_target(); everything else
    // goes through the encoder->vic mailbox below. The supervisor's first_lock / force_reapply are
    // parameters, not mailbox pre-sets.
    static arb_state_t arb_st = {0};
    // The Arbiter reports for duty -- once per boot, the first time it runs.
    static bool s_arb_announced = false;
    if (!s_arb_announced) {
        s_arb_announced = true;
        debug_log(LOG_ARB_WORT);
    }
    // Frozen (WRITE_FREEZE) means pure standalone: no new packet is applied, but the BIOS
    // latches still hold the last applied mode, so the snapshot shows no BIOS state at all --
    // otherwise L1 kept re-picking the pre-freeze mode and standalone detection never ran.
    const bool bench_pure_standalone = smbus_get_apply_frozen();
    uint32_t mdw = !bench_pure_standalone ? bios_last_mode_dword() : 0;
    adv_aspect_ratio ua = user_config_get()->aspect_ratio;
    uint32_t eeprom_vset = 0;
    bool eeprom_ws_valid = xbox_eeprom_get_video_settings(&eeprom_vset);
    uint32_t eeprom_vstd = 0;
    bool eeprom_std_valid = xbox_eeprom_get_video_standard(&eeprom_vstd);
    // Live encoder aspect, all three encoders (Xcalibur's is register 0x07).
    bool enc_widescreen = false;
    bool enc_ws_valid = xbox_encoder_get_wss_widescreen(xb_encoder, &enc_widescreen);
    // An HPD-low reset leaves the chip reading VIC 0 (and 0x17 clear) with the mode still
    // applied -- the reconnect's re-push restores it. The arbiter judges what's applied: on
    // VIC 0 it saw nothing to contradict, and a handoff re-pushed a 720 mode the width had
    // already ruled out, ahead of L5's 640. Known from the disconnect, or from
    // the readback (a drop too short to see).
    XboxVideoTiming applied;
    bool applied_ws;
    const bool chip_lost_mode = (adv7511_chip_lost_timing() || active_vic == VIC_00_VIC_Unavailable) &&
                                adv7511_applied_mode(&applied, &applied_ws);
    if (chip_lost_mode) active_vic = applied.vic;
    arb_snapshot_t snap = {
        .hw_vic = current_hw_vic, .active_vic = active_vic, .pclk_hz = pclk_hz,
        // VIC 1's 4:3 and anamorphic 16:9 share a VIC number, so read the applied aspect
        // back from 0x17[1] (set by the apply exactly when the AVI aspect is 16:9).
        .active_is_anamorphic = (active_vic == VIC_01_VGA_640x480_4_3) &&
                                (chip_lost_mode ? applied_ws : (adv7511_read_register(0x17) & 0x02) != 0),
        .pclk_stable = adv7511_pixel_clock_stable(),
        .width_wide = sd_width_is_wide(), .width_confirmed = sd_width_is_confirmed(),
        .width_stale_held = sd_width_is_ihw_stale_held(),
        .width_predates_bios = sd_width_predates_bios(),
        .tmds_cycle_busy = adv7511_tmds_cycle_busy(),
        // Nor with no packet behind it: a freeze drops it (WRITE_FREEZE), and after the
        // freeze L1 held the pre-freeze mode with nothing to re-apply it from -- a reconnect
        // stayed black. The handoff takes it over.
        .bios_confirmed = !bench_pure_standalone && mdw != 0 && adv7511_active_mode_is_bios(),
        .confirmed_mode_index = !bench_pure_standalone ? get_active_bios_mode_index() : 0xFF,
        .bios_widescreen = (mdw & 0x10000000u) != 0,
        .user_aspect = (ua == ASPECT_RATIO_16_9) ? ARB_ASPECT_16_9 :
                       (ua == ASPECT_RATIO_4_3)  ? ARB_ASPECT_4_3  : ARB_ASPECT_AUTO,
        .encoder_id = (uint8_t)xb_encoder, .first_lock = first_lock,
        .now_ms = HAL_GetTick(),
        .eeprom_ws_valid = eeprom_ws_valid,
        .eeprom_widescreen = eeprom_ws_valid && xbox_eeprom_decode_widescreen(eeprom_vset),
        .eeprom_std_valid = eeprom_std_valid,
        .eeprom_std_is_pal = eeprom_std_valid &&
            xbox_eeprom_video_standard_is_pal(xbox_eeprom_decode_video_standard(eeprom_vstd)),
        .eeprom_refresh_60hz = eeprom_std_valid &&
            xbox_eeprom_resolve_refresh(eeprom_vstd, eeprom_ws_valid ? eeprom_vset : 0u)
                == XBOX_EEPROM_REFRESH_60HZ,
        .enc_ws_valid = enc_ws_valid,
        .enc_widescreen = enc_widescreen,
        .xcal_mode_index = (xb_encoder == ENCODER_XCALIBUR) ? xcal_index_on_clock(pclk_hz) : 0,
        .focus_sdtv = xb_encoder == ENCODER_FOCUS && focus_sd_is_sdtv_family(),
        .enc_hd_vic = xbox_encoder_hd_vic(xb_encoder),
    };
    // A mode change under way (mode_change_update()). Each burst voids the blind fallback's
    // latch, raster and aspect -- re-applied after a reconnect, it put the old 16:9 640x480 out
    // ahead of the new raster's 4:3 (or, latched mid-change, the previous mode set's 4:3) --
    // and its candidate: L6 holds until a verdict on the new raster has
    // held.
    const bool change_open = mode_change_update(&snap, force_reapply);
    if (s_burst_began) {
        arb_st.blind_fb_valid = false;
        arb_st.blind_fb_cand_vic = 0;
    }
    // The arbiter decides every pass, its debounces running on. A decision the change holds
    // back isn't made: the arbiter goes back to its state before it, and decides again -- at
    // once -- on the change's complete data. (Taken as made, its debounce restarted from zero.)
    const arb_state_t arb_before = arb_st;
    vic_decision_t d = decide_target_vic(&snap, &arb_st);
    // Rising edge of the arbiter's hold-stuck escape -- see arb_state_t.hold_stuck_escaped.
    static bool s_hold_stuck_logged = false;
    if (arb_st.hold_stuck_escaped && !s_hold_stuck_logged) {
        debug_log(LOG_ARB_L6_STUCK,
                  (unsigned long)ARB_HOLD_STUCK_TIMEOUT_MS, snap.hw_vic, snap.active_vic,
                  (unsigned long)snap.pclk_hz);
    }
    s_hold_stuck_logged = arb_st.hold_stuck_escaped;
    smbus_note_arbiter_decision((uint8_t)d.source, d.target_vic);
    const bool arb_wants = d.apply;
    if (change_open && d.apply) {
        arb_st = arb_before;
        d.apply = false;
    }
    // Nothing under way: no mode change (nor its verdict grace), no decision wanted, no verdict
    // still to come -- a debounce running (a reconnect re-push of the old VIC 1 went out ahead
    // of its 720x480; a settings re-apply put 640 out 150 ms ahead of L5's 720), or a
    // blind-fallback candidate.
    s_change_settled = !change_open && snap.now_ms - s_change_closed_at >= VERDICT_GRACE_MS;
    s_settled = s_change_settled && !arb_wants &&
        !(arb_st.blind_fb_cand_vic && (arb_st.blind_fb_cand_vic != active_vic ||
                                       arb_st.blind_fb_cand_ana != snap.active_is_anamorphic)) &&
        !arb_st.verdict_pending;
    // The picture's real owner settled: a BIOS with a packet behind it needs only the change
    // over; otherwise standalone owns it (or is about to: the handoff), and nothing of its
    // own may be under way.
    s_owner_settled = s_change_settled && (snap.bios_confirmed || s_settled);
    // BIOS -> standalone handoff: the chip still says BIOS owns the picture, but the
    // arbiter has moved on (the d.source check below). Forces one standalone apply, which
    // clears the BIOS flag, so it fires once. Needs a settled clock.
    bool bios_to_standalone_handoff = adv7511_active_mode_is_bios() &&
                                      snap.pclk_stable && !snap.tmds_cycle_busy &&
                                      snap.pclk_hz > 0;
    // Clock-band changes within one VIC: Conexant runs VIC 1 at four clocks (~24.42/24.94/
    // 26.87/27.56 MHz) and VIC 2/3 at 480p 27.000, 480i 27.378/29.454 and PAL-I 0x04 27.562
    // MHz. arb_pick() only sees VIC changes, so reapply when the clock moves over 150 kHz from
    // the one behind the last apply of the same width class -- above jitter, below the
    // smallest real gap (0x0E -> 0x04, 184 kHz: a 200 kHz trigger kept 0x0E's hs=135 row on
    // 0x04). VIC 0 counts as 640 here: it applies as 640.
    const uint8_t active_class = (active_vic == VIC_00_VIC_Unavailable) ? 1 : sd480_class(active_vic);
    // Not when the clock has left the active family -- or SD altogether (VIC 3 went out on
    // 720p's 74.25 MHz, then the arbiter's VIC 4): that is a mode change, and re-pushing the
    // old VIC first cost a full cycle.
    const bool clock_left_family = arb_clock_contradicts_active_family(&snap);
    if (active_class != 0 && active_class == s_sd480_apply_class && !clock_left_family &&
        snap.pclk_stable && !snap.tmds_cycle_busy && snap.pclk_hz >= ARB_SD_PCLK_MIN_HZ &&
        snap.pclk_hz <= ARB_SD_PCLK_MAX_HZ) {
        if (s_sd480_apply_pclk > 0) {
            const uint32_t pclk_diff = (snap.pclk_hz > s_sd480_apply_pclk) ? (snap.pclk_hz - s_sd480_apply_pclk)
                                                                            : (s_sd480_apply_pclk - snap.pclk_hz);
            if (pclk_diff > 150000UL) {
                force_reapply = true;
            }
        } else if (s_settled) {
            // Applied with no clock (a clock still settling): its timing row was picked blind,
            // and Conexant's VIC 1 rows differ by clock alone -- 0x01's stayed on 0x0F's raster
            // for good. Once the arbiter has nothing under way (a decision of its own may be
            // about to replace the mode), resolve the row the clock calls for: re-apply if it
            // differs, else adopt the clock, so this runs once.
            XboxVideoTiming applied, want = {0};
            bool applied_ws;
            if (adv7511_applied_mode(&applied, &applied_ws)) {
                if (vic_is_576i(applied.vic)) {
                    apply_hd_or_pal576_vic(xb_encoder, applied.vic, snap.pclk_hz, &want);
                } else {
                    sd480_by_vic(xb_encoder, applied.vic, snap.pclk_hz, applied_ws, &want);
                }
                if (want.active_w && adv7511_timing_would_change(&want, applied_ws)) {
                    force_reapply = true;
                } else {
                    s_sd480_apply_pclk = snap.pclk_hz;
                }
            }
        }
    }
    // The row moved under the same VIC and clock (Xcalibur 0x05 <-> 0x12, 0x02 <-> 0x08,
    // 0x01 <-> 0x07, frozen): the old row's VS/HS stayed on the new raster.
    // Standalone-owned only -- a BIOS apply picks the row from the packet. In place when only
    // the vertical start moved (adv7511_xbox.c raster_is_applied()), unless a reconnect wants
    // the re-push too.
    bool row_reapply = false;
    if (xcal_row_pair(snap.xcal_mode_index) && !adv7511_active_mode_is_bios() && snap.pclk_stable &&
        !snap.tmds_cycle_busy && snap.xcal_mode_index != s_xcal_apply_idx &&
        xcal_row_pair(snap.xcal_mode_index) == xcal_row_pair(s_xcal_apply_idx)) {
        row_reapply = !force_reapply;
        force_reapply = true;
    }
    // The aspect setting an entry is written under (see the mailbox below).
    static uint8_t s_entry_aspect;
    if (!(encoder->vic & ADV7511_VIC_CHANGED)) {
        s_entry_aspect = user_config_get()->aspect_ratio;
    }
    // Standalone decisions feed the mailbox (BIOS ones are applied by
    // bios_reconcile_pending_target()).
    if (d.source != ARB_SRC_BIOS &&
        !(encoder->vic & ADV7511_VIC_CHANGED)) {
        // A re-push waits out a mode change or a verdict under way (the arbiter's apply then
        // takes its place), and so does a relock re-apply during a change.
        if ((force_reapply && (change_open || !s_settled)) || (first_lock && change_open)) {
            want_repush();
            force_reapply = first_lock = false;
        } else if (s_repush_wanted && s_settled) {
            s_repush_wanted = false;
            force_reapply = adv7511_apply_count() == s_repush_applies;
        }
        if (d.apply) {
            // 16:9 wanted = widescreen || anamorphic (exclusive by construction): VIC 1
            // carries its 16:9 in d.anamorphic, and packing d.widescreen alone lost it, so
            // the apply and arb_pick() disagreed and re-applied every loop.
            encoder->vic = ADV7511_VIC_CHANGED | ADV7511_VIC_INPLACE_OK | d.target_vic |
                           ((d.widescreen || d.anamorphic) ? ADV7511_VIC_WIDESCREEN : 0);
            // One line per real standalone decision: the level (0 hold, 1 BIOS, 3 Path
            // A, 4 HW VIC, 5 width, 6 blind), the detector and active VICs,
            // the clock and its stability, the width verdict, and where the width came
            // from (wsrc).
            debug_log(LOG_ARB_PICK,
                      (int)d.source, d.target_vic, snap.hw_vic, snap.active_vic,
                      (unsigned)snap.pclk_hz, snap.pclk_stable, snap.width_wide,
                      snap.width_confirmed, sd_width_last_source());
            note_sd480_apply(sd480_class(d.target_vic), snap.pclk_hz);
        } else if (first_lock && d.target_vic != VIC_00_VIC_Unavailable) {
            // First lock, and the arbiter's VIC already matches the applied one (after a
            // soft MCU reset the chip still holds the old mode). Re-apply anyway: the
            // chip reinit gated output, and without this the screen stays black after
            // `rst`. In place when the chip kept its registers (a relock); after an MCU
            // reset the applied timing was invalidated, so it takes the full cycle.
            encoder->vic = ADV7511_VIC_CHANGED | ADV7511_VIC_INPLACE_OK | d.target_vic |
                           ((d.widescreen || d.anamorphic) ? ADV7511_VIC_WIDESCREEN : 0);
            debug_log(LOG_ARB_RELOCK_REAPPLY, d.target_vic);
            note_sd480_apply(sd480_class(d.target_vic), snap.pclk_hz);
        } else if (force_reapply || (bios_to_standalone_handoff && !clock_left_family && s_settled)) {
            // No change wanted, but the mode must be re-pushed (HPD reconnect, a clock
            // move within the VIC: a full cycle, no INPLACE_OK) or taken over from BIOS.
            // Not a takeover of a mode the clock/width already contradicts, nor while the
            // arbiter has a verdict under way (s_settled): its own apply follows and takes
            // over in one cycle (re-pushing the BIOS's Xcalibur 0x0F as VIC 1, then its 0x02
            // as VIC 3, cost two; a freeze re-pushed 16:9 just ahead of the arbiter's 4:3).
            // The aspect is re-resolved here (d.widescreen is unset on a hold), see
            // sd_repush_vic(). A reconnect's VIC 0 is the applied VIC by now (chip_lost_mode).
            bool fr_wide;
            // Nothing applied yet, nothing to re-push: the arbiter's first decision puts out the
            // real mode. (The encoder self-latching on the console's first writes re-pushed VIC
            // 0 as 640x480 ahead of it.)
            if (active_vic != VIC_00_VIC_Unavailable) {
                const uint8_t repush_vic = sd_repush_vic(xb_encoder, active_vic, &fr_wide);
                // A handoff alone (no repair) may go in place: when standalone resolves the
                // picture already up, only ownership changes -- a freeze blinked the TV with a
                // full cycle for the same picture (in every mode). A different raster
                // (BIOS 0x0F -> standalone's 0x02) or a chip that lost it still cycles.
                encoder->vic = ADV7511_VIC_CHANGED | repush_vic |
                               (fr_wide ? ADV7511_VIC_WIDESCREEN : 0) |
                               (force_reapply && !row_reapply ? 0 : ADV7511_VIC_INPLACE_OK);
                // The re-pushed VIC's class: the band check compares against it next (taken
                // from a reconnect's VIC 0 as 640, it never matched the 720 class again --
                // Focus 480p -> 0x02 kept 480p's hs=120 after an HPD pulse).
                note_sd480_apply(sd480_class(repush_vic), snap.pclk_hz);
            }
        }
    }

    if (encoder->vic & ADV7511_VIC_CHANGED) {
        // ADV7511_VIC_MASK, not ADV7511_VIC_CHANGED_CLEAR: bits 5-6 carry the widescreen and
        // in-place flags.
        uint8_t target_vic = encoder->vic & ADV7511_VIC_MASK;
        // The aspect decided when the entry was written -- not re-derived, since WSS/EEPROM
        // can change during the waits below.
        bool mailbox_widescreen = (encoder->vic & ADV7511_VIC_WIDESCREEN) != 0;
        const bool mailbox_inplace_ok = (encoder->vic & ADV7511_VIC_INPLACE_OK) != 0;

        // A mode change started while the entry waited (a TMDS cycle's tail): it is the old
        // raster's -- a reconnect re-push put 720x480 out on a new 576
        // raster. A re-push takes its place, which the arbiter's verdict on the new
        // raster supersedes.
        if (change_open) {
            encoder->vic &= ADV7511_VIC_CHANGED_CLEAR;
            want_repush();
            return false;
        }
        // VIC 2 on a settled HD clock: the raster it was meant for is gone (a reconnect re-push
        // put 720x480 on a switch to 1080i). The arbiter decides anew. (VIC 2 also used to wait
        // here, at most 1 s, for a settled clock and a confirmed width; waiting never changed
        // the outcome, so it was dropped.)
        if (target_vic == VIC_02_480p_60__4_3 && adv7511_pixel_clock_stable() && pclk_hz >= ARB_HD_PCLK_MIN_HZ) {
            encoder->vic &= ADV7511_VIC_CHANGED_CLEAR;
            return false;
        }
        // A re-push (no INPLACE_OK) re-resolves its aspect now, and so does an SD decision
        // written under another aspect setting: an aspect write or a defaults reset while the
        // entry waited went out old-aspect, then the arbiter's in place.
        if (!mailbox_inplace_ok ||
            (user_config_get()->aspect_ratio != s_entry_aspect && !vic_is_hd(target_vic))) {
            target_vic = sd_repush_vic(xb_encoder, target_vic, &mailbox_widescreen);
        }

        // A cycle's tail still running: retry next pass (the CHANGED flag stays set).
        if (adv7511_tmds_cycle_busy()) {
            return false;
        }

        encoder->vic &= ADV7511_VIC_CHANGED_CLEAR;

        // Arbiter decisions may be aspect-only on the same raster (VIC 1 anamorphic <-> 4:3,
        // 2 <-> 3, 17 <-> 18): defer the gate so those apply in place. Relock / re-push
        // entries have no INPLACE_OK and always cycle.
        if (mailbox_inplace_ok) {
            adv7511_tmds_cycle_begin_deferred();
        } else {
            adv7511_tmds_cycle_begin();
        }
        // No clock yet (just after a reconnect, the ACR restarting): for a VIC of the width
        // class last applied, the clock behind that apply -- the raster hasn't changed, and a
        // blind pick put Conexant's 0x01 row on 0x0F's raster. A new raster's row gets
        // re-checked once its clock settles (above).
        const uint32_t row_pclk = (pclk_hz || sd480_class(target_vic) != s_sd480_apply_class)
                                ? pclk_hz : s_sd480_apply_pclk;
        apply_standalone_by_vic(xb_encoder, target_vic, row_pclk, mailbox_widescreen);
        adv7511_tmds_cycle_end();
        return true;
    }
    return false;
}

// Reapplies the active standalone mode now (reapply_active_mode()'s standalone branch, e.g.
// after a hotplug or a setting change). Uses the applied internal VIC, not the detector: 0x3E
// can stay blind on the PAL hybrid raster, which would make Path A's VIC unreachable. Nor
// the chip's readback, which says VIC 0 for "nothing applied" and after an HPD-low reset:
// re-applied as VIC 0, that put out 640x480 ahead of the real mode. With nothing applied
// there is nothing to re-apply -- the first decision takes the new setting.
void reapply_active_video_mode(const xbox_encoder xb_encoder) {
    XboxVideoTiming applied;
    bool applied_ws;
    if (!adv7511_applied_mode(&applied, &applied_ws)) {
        return;
    }
    bool wide;
    const uint8_t vic = sd_repush_vic(xb_encoder, applied.vic, &wide);
    apply_standalone_by_vic(xb_encoder, vic, adv7511_get_pixel_clock_hz(), wide);
}

// Resolves and applies one standalone mode. anamorphic: 640 geometry and its 4:3 VIC (1), with
// only the AVI aspect saying 16:9.
// `mode` is one of the XBOX_VIDEO_* constants its two callers pass.
static void set_video_mode_vic_ex(const xbox_encoder xb_encoder, const uint8_t mode, const bool widescreen, const bool interlaced, const bool anamorphic, const uint32_t pclk_hz, XboxVideoTiming *dry) {
    // Timing/VIC selection sees 4:3 for the anamorphic case; only the InfoFrame flag is 16:9.
    const bool timing_wide = widescreen && !anamorphic;

    // Focus: a fresh IHO (interlaced SD only -- see sd_width_debounce_update()) names the
    // console's own mode within the family the arbiter chose, so standalone applies the BIOS
    // packet's row and calibration slot rather than a synthetic or a sibling: NTSC 0x01 vs
    // 0x14 differ in profile, PAL 0x04 vs 0x15 in VS delay.
    uint8_t focus_idx = 0;
    if (xb_encoder == ENCODER_FOCUS) {
        const uint8_t iho = s_sdw.focus_iho;
        if (mode == XBOX_VIDEO_480p_640) {
            focus_idx = iho == 182 ? 0x01 : iho == 151 ? 0x0F : iho == 146 ? 0x03 : iho == 115 ? 0x10 : 0;
        } else if (mode == XBOX_VIDEO_480p_720) {
            focus_idx = iho == 143 ? 0x02 : iho == 103 ? 0x04 : 0;
        } else if (mode == XBOX_VIDEO_576i_640) {
            focus_idx = iho == 146 ? 0x05 : iho == 115 ? 0x12 : 0;
        }
    }
    XboxVideoTiming timing;
    if (focus_idx ? !xbox_get_video_timing(xb_encoder, focus_idx, timing_wide, &timing)
                  : !xbox_get_standalone_timing(xb_encoder, mode, timing_wide, interlaced, pclk_hz, &timing)) {
        debug_log(LOG_ARB_BAD_TIMING);
        return;
    }
    // A 720x480 apply below the Path A threshold (23.95 MHz) takes that encoder's PAL 0x04 HS
    // delay instead of the table's: the 480-line/50 Hz hybrid. On Xcalibur this is Path A
    // (decide_target_vic() L3); on Focus it is the same ~22.68 MHz raster, reached through
    // L6/the width signal.
    if (mode == XBOX_VIDEO_480p_720 &&
        pclk_hz > 0 && pclk_hz < ARB_PATH_A_PCLK_HIGH_HZ) {
        timing.delay_hs = xbox_pal_720x480_hs_delay(xb_encoder);
    }
    // Anamorphic keeps timing.vic at VIC 1: it is the internal VIC the arbiter compares
    // against, so changing it would keep the decision from ever converging.
    if (dry) {
        *dry = timing;
        return;
    }

    log_autodetect(&timing, widescreen || anamorphic, anamorphic, interlaced, pclk_hz);

    adv7511_set_aspect_known(sd_aspect_known(xb_encoder));
    adv7511_apply_video_timing(&timing, widescreen || anamorphic, false);
}
