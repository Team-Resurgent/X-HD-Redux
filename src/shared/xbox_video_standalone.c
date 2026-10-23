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
                               // sd_width_last_source_str())
    uint8_t  focus_iho;       // Focus IHO as of the last update if fresh, else 0
} s_sdw = { .candidate = 0xFF };

// Which input produced the last width sample, for the [ARB] line's wsrc= field.
const char *sd_width_last_source_str(void) {
    switch (s_sdw.source) {
        case 1:  return "focus-ihw";
        case 2:  return "cx-hact";
        case 3:  return "focus-ihw-stale-held";
        case 4:  return "xcal-reg";
        default: return "pclk";
    }
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
static int xcal_mode_index_classify_instant(uint8_t idx) {
    const uint8_t m = xbox_mode_flags(idx, ENCODER_XCALIBUR);
    if ((m & MODE_FAM_MASK) == FAM_NONE) return -1;
    return (m & MODE_640) ? 0 : 1;
}

// A VIC's fixed CEA aspect: 1 for the 16:9 VICs of the 2/3, 17/18, 21/22 pairs, 0 for the 4:3
// ones -- a reapply must not second-guess those. -1 for VIC 1 (no 16:9 sibling) and anything
// else: ask sd_resolve_widescreen().
static int sd_vic_rigid_aspect(uint8_t vic) {
    const bool pair = vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9 ||
                      vic_is_576p(vic) || vic_is_576i(vic);
    return pair ? vic_is_16_9(vic) : -1;
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
    // have_fs_ihw drops to false when the IHW value has gone stale (staleness guard below).
    bool have_fs_ihw = is_focus;
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
    bool cx_raster_int;
    int cx_raster_prog = xbox_encoder_get_int_prog(ENCODER_CONEXANT, &cx_raster_int) ?
                         (cx_raster_int ? 0 : 1) : -1;
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
        (!xbox_encoder_get_int_prog(ENCODER_FOCUS, &fs_interlaced) || fs_interlaced);
    int fs_width_inst = -1;
    if (have_fs_iho) {
        fs_width_inst = focus_iho_is_720_wide(fs_iho) ? 1 : 0;
    } else if (have_fs_ihw) {
        fs_width_inst = focus_ihw_classify_instant(fs_ihw);
    }
    bool have_focus_width = (have_fs_iho || have_fs_ihw);
    s_sdw.focus_iho = have_fs_iho ? fs_iho : 0;

    int xcal_width_inst = (xb_encoder == ENCODER_XCALIBUR) ?
        xcal_mode_index_classify_instant(xbox_xcal_mode_index()) : -1;
    // Physics guard: a clock in NTSC's narrow band (~24.545 MHz, 0x07) can't be a wide raster,
    // whatever the index table says -- a table mistake then fails safe. Band-limited because
    // PAL 0x04 is genuinely wide at 23.400 MHz on Xcalibur.
    if (xb_encoder == ENCODER_XCALIBUR && xcal_width_inst == 1 &&
        pclk_hz >= 24000000UL && pclk_hz < SD_WIDTH_THRESHOLD_NARROW_HZ) {
        xcal_width_inst = 0;
    }

    int inst = (xb_encoder == ENCODER_FOCUS) ?
                   (have_focus_width ? fs_width_inst : (focus_pclk_ambiguous ? -1 : pclk_inst)) :
               (xb_encoder == ENCODER_XCALIBUR) ?
                   (xcal_width_inst >= 0 ? xcal_width_inst : pclk_inst) :
               use_cx_hactive ? cx_hactive_inst : pclk_inst;
    uint8_t sample = (inst >= 0) ? (uint8_t)inst : s_sdw.last_sample;
    s_sdw.last_sample = sample;
    s_sdw.source = (xb_encoder == ENCODER_FOCUS && have_focus_width) ? 1 :
                   (focus_pclk_ambiguous) ? 3 :
                   (xb_encoder == ENCODER_XCALIBUR && xcal_width_inst >= 0) ? 4 :
                   use_cx_hactive ? 2 : 0;

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
    if (!s_sd_int_prog_stale && xbox_encoder_get_int_prog(ENCODER_CONEXANT, &is_int)) {
        return is_int;
    }
    // Stale or never seen: the clock. Both real 480i rates are unambiguous once PAL 720 is
    // excluded -- ~27.378 MHz (HD Test App) and ~29.454 MHz (retail BT.601 480i). Staleness is
    // common here: 640 NTSC baselines 0x2E=0x00 at ~24.42 MHz, a later 480i at ~27.378 MHz
    // doesn't rewrite the same value, and the ~3 MHz jump marks it stale.
    if ((pclk_hz >= 27250000UL && pclk_hz < 27450000UL) ||
        (pclk_hz >= 28500000UL && pclk_hz <= 30500000UL)) {
        return true;
    }
    return false;
}

static void set_video_mode_vic_ex(const xbox_encoder xb_encoder, const uint8_t mode, const bool widescreen, const bool interlaced, const bool anamorphic, const uint32_t pclk_hz);

// Single [AUTODETECT] line, shared by every set_video_mode_vic_ex() call: what is being
// applied, in terms that can't be mistaken for the wire VIC (that's the "Actual VIC Sent :"
// line right after, which can honestly be 0 for a non-CEA raster such as 640x480p at
// 24.545MHz). p/i is the scan put on HDMI (only 1080i is interlaced; every SD raster on the
// pixel bus is progressive). "timing VIC" is the CEA family the timing row was built from
// (geometry, CSC, AVI). sdtv marks the SDTV-family timing row (HS delay / clock band) -- SD
// only: the caller's flag is plain "interlaced" for 1080i.
static void log_autodetect(const XboxVideoTiming *t, bool wide, bool anamorphic, bool sdtv_row, uint32_t pclk_hz) {
    debug_log("[AUTODETECT] %ux%u%c %s%s hs=%u timing VIC %u%s",
              t->active_w, t->active_h, t->interlaced_offset ? 'i' : 'p',
              wide ? "16:9" : "4:3", anamorphic ? " anamorphic" : "", t->delay_hs, t->vic,
              (sdtv_row && t->active_w < 1280) ? " sdtv" : "");   // HD passes it as "interlaced"
    if (pclk_hz) {
        const uint32_t khz = pclk_hz / 1000u;
        debug_log(" %u.%03uMHz", (unsigned)(khz / 1000u), (unsigned)(khz % 1000u));
    }
    debug_log("\r\n");
}

// HD (VIC 4/5, always 16:9) and PAL 576 (17/18 720-wide, 21/22 the 640-wide placeholder)
// applies; any other VIC is ignored. The aspect is the VIC's own (17/21 are 4:3, 18/22 16:9)
// -- re-resolving it here could contradict the arbiter's choice and re-apply forever.
static void apply_hd_or_pal576_vic(const xbox_encoder xb_encoder, uint8_t vic, uint32_t pclk_hz) {
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
    set_video_mode_vic_ex(xb_encoder, mode, wide, interlaced, false, pclk_hz);
}

// The one VIC -> standalone apply mapping, used by stand_alone_loop() and
// reapply_active_video_mode(). VIC 2 is trusted as 720-wide (the arbiter's width debounce
// decided it). mailbox_widescreen is the arbiter's aspect, carried through the mailbox -- not
// re-resolved here, so a WSS/EEPROM change during a deferred apply can't disagree with the
// VIC already chosen.
void apply_standalone_by_vic(const xbox_encoder xb_encoder, uint8_t vic,
                              uint32_t pclk_hz, bool mailbox_widescreen) {
    // SDTV-family (interlaced-row) pick for the 480-line VICs; Xcalibur has none.
    const bool interlaced = (xb_encoder == ENCODER_FOCUS) ? focus_sd_is_sdtv_family()
                          : (xb_encoder == ENCODER_CONEXANT) ? cx_sd_is_sdtv_family(vic, pclk_hz)
                          : false;
    if (vic == VIC_00_VIC_Unavailable || vic == VIC_01_VGA_640x480_4_3 ||
        vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9) {
        // 480-line SD: 720 geometry for VIC 2/3, else 640, where a 16:9 want is anamorphic.
        // Path A's HS delay (Xcalibur VIC 2 in its window) comes from set_video_mode_vic_ex().
        const bool geom_wide = (vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9);
        set_video_mode_vic_ex(xb_encoder, geom_wide ? XBOX_VIDEO_480p_720 : XBOX_VIDEO_480p_640,
                              mailbox_widescreen, interlaced, !geom_wide && mailbox_widescreen,
                              pclk_hz);
    } else {
        // HD and PAL 576 (VIC 4/5, 17/18/21/22). VIC 21/22 must land here too, or a stable
        // 576i detector reading never gets applied and the 30 ms debounce re-fires forever.
        apply_hd_or_pal576_vic(xb_encoder, vic, pclk_hz);
    }
}

// The clock behind the last 480-line SD apply and its width class (sd480_class()), for
// stand_alone_loop()'s clock-move reapply. One at a time: an apply of anything else clears it.
static uint32_t s_sd480_apply_pclk = 0;
static uint8_t  s_sd480_apply_class = 0;

// 1 = 640 (VIC 1), 2 = 720x480 (VIC 2/3), 0 = anything else.
static uint8_t sd480_class(uint8_t vic) {
    return (vic == VIC_01_VGA_640x480_4_3) ? 1 :
           (vic == VIC_02_480p_60__4_3 || vic == VIC_03_480p_60_16_9) ? 2 : 0;
}

static void note_sd480_apply(uint8_t cls, uint32_t pclk_hz) {
    s_sd480_apply_class = cls;
    s_sd480_apply_pclk = cls ? pclk_hz : 0;
}

// When a queued VIC 2 apply started waiting for its width (0 = not waiting); see the wait in
// stand_alone_loop().
static uint32_t s_width_wait_start = 0;

// A BIOS packet takes over: drop a standalone apply still queued in the mailbox, and the
// width wait it may be in -- left running, the next VIC 2 entry would count from the old
// start and skip its wait.
void stand_alone_cancel_queued(adv7511 *encoder) {
    encoder->vic &= ADV7511_VIC_CHANGED_CLEAR;
    s_width_wait_start = 0;
}

// One standalone pass: build the arbiter snapshot, decide, and apply through the encoder->vic
// mailbox. True if a mode was applied this call (the caller then un-gates output); false if it
// was a no-op or is still waiting (blind detector, a debounce, the width wait).
//
// first_lock: the first PLL-settle resolve of a boot -- active_vic is still the init value, so
//   the arbiter adopts the detector's reading at once (L4, no debounce).
// force_reapply: re-push the current mode even if its VIC is unchanged (HPD reconnect /
//   encoder self-latch reset AVI/CSC/packet state that a bare "no change" verdict skips).
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
        debug_log("[ARB] Wort Wort Wort.\r\n");
    }
    // Frozen (WRITE_FREEZE) means pure standalone: no new packet is applied, but the BIOS
    // latches still hold the last applied mode, so the snapshot shows no BIOS state at all --
    // otherwise L1 kept re-picking the pre-freeze mode and standalone detection never ran.
    const bool bench_pure_standalone = smbus_get_apply_frozen();
    uint32_t mdw = !bench_pure_standalone ? bios_last_mode_dword() : 0;
    bool bpend = !bench_pure_standalone ? bios_reconcile_target_pending() : false;
    adv_aspect_ratio ua = user_config_get()->aspect_ratio;
    uint32_t eeprom_vset = 0;
    bool eeprom_ws_valid = xbox_eeprom_get_video_settings(&eeprom_vset);
    uint32_t eeprom_vstd = 0;
    bool eeprom_std_valid = xbox_eeprom_get_video_standard(&eeprom_vstd);
    // Live encoder aspect, all three encoders (Xcalibur's is register 0x07).
    bool enc_widescreen = false;
    bool enc_ws_valid = xbox_encoder_get_wss_widescreen(xb_encoder, &enc_widescreen);
    uint16_t cx_snap_hactive = 0;
    const bool have_cx_snap_hactive = (xb_encoder == ENCODER_CONEXANT) && xbox_cx_get_hactive(&cx_snap_hactive);
    uint8_t fs_snap_iho = 0;
    const bool have_fs_snap_iho = (xb_encoder == ENCODER_FOCUS) && xbox_focus_get_iho(&fs_snap_iho);
    arb_snapshot_t snap = {
        .hw_vic = current_hw_vic, .active_vic = active_vic, .pclk_hz = pclk_hz,
        // VIC 1's 4:3 and anamorphic 16:9 share a VIC number, so read the applied aspect
        // back from 0x17[1] (set by the apply exactly when the AVI aspect is 16:9).
        .active_is_anamorphic = (active_vic == VIC_01_VGA_640x480_4_3) &&
                                ((adv7511_read_register(0x17) & 0x02) != 0),
        .pclk_stable = adv7511_pixel_clock_stable(),
        .width_wide = sd_width_is_wide(), .width_confirmed = sd_width_is_confirmed(),
        .width_stale_held = sd_width_is_ihw_stale_held(),
        .tmds_cycle_busy = adv7511_tmds_cycle_busy(),
        .bios_confirmed = !bench_pure_standalone && adv7511_active_mode_is_bios(),
        .bios_pending = bpend,
        .pending_mode_index = bpend ? xbox_mode_index(mdw) : 0xFF,
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
        .xcal_mode_index = (xb_encoder == ENCODER_XCALIBUR) ? xbox_xcal_mode_index() : 0,
        .width_reg = have_cx_snap_hactive ? cx_snap_hactive :
                     have_fs_snap_iho ? fs_snap_iho : 0,
        .width_reg_epoch = have_cx_snap_hactive ? xbox_cx_get_hactive_epoch() :
                           have_fs_snap_iho ? xbox_focus_get_ihw_epoch() : 0,
    };
    vic_decision_t d = decide_target_vic(&snap, &arb_st);
    // Rising edge of the arbiter's hold-stuck escape -- see arb_state_t.hold_stuck_escaped.
    static bool s_hold_stuck_logged = false;
    if (arb_st.hold_stuck_escaped && !s_hold_stuck_logged) {
        debug_log("[ARB] L6 hold stuck %lums -- released (hw=0x%02X ac=0x%02X pclk=%lu)\r\n",
                  (unsigned long)ARB_HOLD_STUCK_TIMEOUT_MS, snap.hw_vic, snap.active_vic,
                  (unsigned long)snap.pclk_hz);
    }
    s_hold_stuck_logged = arb_st.hold_stuck_escaped;
    smbus_note_arbiter_decision((uint8_t)d.source, d.target_vic);
    // BIOS -> standalone handoff: the chip still says BIOS owns the picture, but the
    // arbiter has moved on (the d.source check below). Forces one standalone apply, which
    // clears the BIOS flag, so it fires once. Needs a settled clock.
    bool bios_to_standalone_handoff = adv7511_active_mode_is_bios() &&
                                      snap.pclk_stable && !snap.tmds_cycle_busy &&
                                      snap.pclk_hz > 0;
    // Clock-band changes within one VIC: Conexant runs VIC 1 at four clocks (~24.42/24.94/
    // 26.87/27.56 MHz) and VIC 2/3 at 480p 27.000 and 480i 27.378/29.454 MHz. arb_pick()
    // only sees VIC changes, so reapply when the clock moves over 200 kHz from the one
    // behind the last apply of the same width class -- above jitter, below the smallest real
    // gap (~378 kHz). VIC 0 counts as 640 here: it applies as 640.
    const uint8_t active_class = (active_vic == VIC_00_VIC_Unavailable) ? 1 : sd480_class(active_vic);
    // Not when the clock has left the active family: that is a mode change in progress,
    // and re-pushing the old VIC first cost a full cycle.
    const bool clock_left_family = arb_clock_contradicts_active_family(&snap, &arb_st);
    if (active_class != 0 && active_class == s_sd480_apply_class && !clock_left_family &&
        snap.pclk_stable && !snap.tmds_cycle_busy && snap.pclk_hz > 0 && s_sd480_apply_pclk > 0) {
        const uint32_t pclk_diff = (snap.pclk_hz > s_sd480_apply_pclk) ? (snap.pclk_hz - s_sd480_apply_pclk)
                                                                        : (s_sd480_apply_pclk - snap.pclk_hz);
        if (pclk_diff > 200000UL) {
            force_reapply = true;
        }
    }
    // Standalone decisions feed the mailbox (BIOS ones are applied by
    // bios_reconcile_pending_target()).
    if (d.source != ARB_SRC_BIOS && d.source != ARB_SRC_BIOS_WAIT &&
        !(encoder->vic & ADV7511_VIC_CHANGED)) {
        if (d.apply) {
            // 16:9 wanted = widescreen || anamorphic (exclusive by construction): VIC 1
            // carries its 16:9 in d.anamorphic, and packing d.widescreen alone lost it, so
            // the apply and arb_pick() disagreed and re-applied every loop.
            encoder->vic = ADV7511_VIC_CHANGED | ADV7511_VIC_INPLACE_OK | d.target_vic |
                           ((d.widescreen || d.anamorphic) ? ADV7511_VIC_WIDESCREEN : 0);
            // One line per real standalone decision: the level (0 hold, 1 BIOS, 2 BIOS
            // wait, 3 Path A, 4 HW VIC, 5 width, 6 blind), the detector and active VICs,
            // the clock and its stability, the width verdict, and where the width came
            // from (wsrc).
            debug_log("[ARB] L%d -> int VIC %u (hw=0x%02X ac=0x%02X pclk=%lu pcs=%d ww=%d wc=%d wsrc=%s)\r\n",
                      (int)d.source, d.target_vic, snap.hw_vic, snap.active_vic,
                      (unsigned long)snap.pclk_hz, snap.pclk_stable, snap.width_wide,
                      snap.width_confirmed, sd_width_last_source_str());
            note_sd480_apply(sd480_class(d.target_vic), snap.pclk_hz);
        } else if (first_lock && d.target_vic != VIC_00_VIC_Unavailable) {
            // First lock, and the arbiter's VIC already matches the applied one (after a
            // soft MCU reset the chip still holds the old mode). Re-apply anyway: the
            // chip reinit gated output, and without this the screen stays black after
            // `rst`. In place when the chip kept its registers (a relock); after an MCU
            // reset the applied timing was invalidated, so it takes the full cycle.
            encoder->vic = ADV7511_VIC_CHANGED | ADV7511_VIC_INPLACE_OK | d.target_vic |
                           ((d.widescreen || d.anamorphic) ? ADV7511_VIC_WIDESCREEN : 0);
            debug_log("[ARB] relock reapply int VIC %u\r\n", d.target_vic);
            note_sd480_apply(sd480_class(d.target_vic), snap.pclk_hz);
        } else if (force_reapply || (bios_to_standalone_handoff && !clock_left_family)) {
            // No change wanted, but the mode must be re-pushed (HPD reconnect, a clock
            // move within the VIC) or taken over from BIOS. Full cycle (no INPLACE_OK).
            // Not a takeover of a mode the clock/width already contradicts: the arbiter's
            // own apply follows and takes over in one cycle (re-pushing the BIOS's Xcalibur
            // 0x0F as VIC 1, then its 0x02 as VIC 3, cost two).
            // The aspect is re-resolved here (d.widescreen is unset on a hold): the VIC's
            // fixed CEA aspect first -- sd_resolve_widescreen() alone could stretch a 4:3
            // VIC 2/17/21 to 16:9 on a Widescreen=Y console. VIC 0 falls to VIC 1.
            int rigid_aspect = sd_vic_rigid_aspect(active_vic);
            bool fr_wide = (rigid_aspect >= 0) ? (rigid_aspect == 1)
                : sd_resolve_widescreen(xb_encoder, active_vic == VIC_03_480p_60_16_9);
            encoder->vic = ADV7511_VIC_CHANGED | active_vic |
                           (fr_wide ? ADV7511_VIC_WIDESCREEN : 0);
            note_sd480_apply(active_class, snap.pclk_hz);
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

        // VIC 2 is width-ambiguous: wait (at most 1 s) for a settled clock -- it reads 0 for
        // ~500 ms after a disruption -- and a confirmed width before applying, rather than
        // applying a guess and correcting it.
        if (target_vic == VIC_02_480p_60__4_3) {
            bool width_confirmed = sd_width_is_confirmed();
            if (s_width_wait_start == 0) {
                s_width_wait_start = HAL_GetTick();
            }
            uint32_t waited = HAL_GetTick() - s_width_wait_start;
            if (waited < 1000 && (!adv7511_pixel_clock_stable() || !width_confirmed)) {
                return false;
            }
            // Clock settled and width confirmed, or the 1000ms bounded wait expired --
            // fall through and apply with the best-available guess.
        }
        s_width_wait_start = 0;

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
        apply_standalone_by_vic(xb_encoder, target_vic, pclk_hz, mailbox_widescreen);
        adv7511_tmds_cycle_end();
        return true;
    }
    return false;
}

// Reapplies the active standalone mode now (reapply_active_mode()'s standalone branch, e.g.
// after a hotplug or a setting change). Uses the applied internal VIC, not the detector: 0x3E
// can stay blind on the PAL hybrid raster, which would make Path A's VIC unreachable.
void reapply_active_video_mode(const xbox_encoder xb_encoder) {
    uint8_t vic = adv7511_read_active_vic();
    // The VIC's fixed aspect first; sd_resolve_widescreen() only for VIC 1 -- otherwise a 4:3
    // VIC 2/17/21 got stretched to 16:9 on a Widescreen=Y console after a plain hotplug.
    int rigid_aspect = sd_vic_rigid_aspect(vic);
    bool ra_wide = (rigid_aspect >= 0) ? (rigid_aspect == 1)
                                       : sd_resolve_widescreen(xb_encoder, vic == VIC_03_480p_60_16_9);
    apply_standalone_by_vic(xb_encoder, vic, adv7511_get_pixel_clock_hz(), ra_wide);
}

// Resolves and applies one standalone mode. anamorphic: 640 geometry and its 4:3 VIC (1), with
// only the AVI aspect saying 16:9.
static void set_video_mode_vic_ex(const xbox_encoder xb_encoder, const uint8_t mode, const bool widescreen, const bool interlaced, const bool anamorphic, const uint32_t pclk_hz) {
    if (mode > XBOX_VIDEO_576i_640) {
        debug_log("Invalid VIC mode\r\n");
        return;
    }

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
        debug_log("[ARB] bad timing\r\n");
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

    log_autodetect(&timing, widescreen || anamorphic, anamorphic, interlaced, pclk_hz);

    adv7511_apply_video_timing(&timing, widescreen || anamorphic, false);
}
