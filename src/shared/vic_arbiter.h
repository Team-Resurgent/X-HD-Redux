#ifndef __VIC_ARBITER_H__
#define __VIC_ARBITER_H__

// --- VIC autodetect/arbitration: the single VIC decision function ---
//
// One pure function takes every input to the VIC decision (BIOS SMBus state, the ADV7511
// line detector, the clock and width classifiers, EEPROM fields, user config) plus a small
// caller-held state block (debounce timers), and returns ONE decision from a strict priority
// ladder: levels are checked top-down and the first that owns the decision returns.
//
// Called from stand_alone_loop(). Pure -- no HAL, I2C or statics -- so the host tests
// (hosttests/run_arbiter_fixtures.c, run_fixtures.c) drive it directly.

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ARB_SRC_HOLD = 0,      // no change -- current output already satisfies the decision
    ARB_SRC_BIOS,          // a BIOS SMBus mode (confirmed or fast-confirmed pending)
    ARB_SRC_BIOS_WAIT,     // a BIOS target is pending but not yet confirmable -> hold, bounded
    ARB_SRC_PATH_A,        // a stable SD signal in the Path A pixel-clock window (see
                           // ARB_PATH_A_PCLK_LOW_HZ/HIGH_HZ below)
    ARB_SRC_HW_VIC,        // the ADV7511 line detector resolved to a usable, incompatible VIC
    ARB_SRC_WIDTH,         // SD narrow/wide correction from the width signal (encoder registers, else the clock)
    ARB_SRC_BLIND_FALLBACK // nothing resolved past the blind timeout -> conservative default
} arb_source_t;

typedef enum { ARB_ASPECT_AUTO = 0, ARB_ASPECT_4_3, ARB_ASPECT_16_9 } arb_aspect_t;

typedef struct {
    // --- live measurement ---
    uint8_t  hw_vic;            // 0x3E >> 2  (VIC_00 == blind)
    uint8_t  active_vic;        // the applied mode's internal VIC (adv7511_read_active_vic())
    bool     active_is_anamorphic; // the applied VIC 1 carries the 16:9 AVI aspect -- VIC 1
                                   // is one number for 4:3 and 16:9, so arb_pick() needs this
                                   // to see an aspect-only change there
    uint32_t pclk_hz;           // CTS-derived; 0 while the ACR is re-acquiring
    bool     pclk_stable;       // adv7511_pixel_clock_stable()
    bool     width_wide;        // sd_width_is_wide()
    bool     width_confirmed;   // sd_width_is_confirmed()
    // The confirmed width is a held-over sample, not a fresh one (Focus: its width signal went
    // stale in the SDTV clock band and the last verdict was kept). Must not override a fresh
    // BIOS packet.
    bool     width_stale_held;
    bool     tmds_cycle_busy;   // an apply cycle's async tail is in flight -> can't apply now

    // --- BIOS / SMBus ---
    bool     bios_confirmed;        // adv7511_active_mode_is_bios() -- last apply was BIOS
    bool     bios_pending;          // a reconcile target is pending
    uint8_t  pending_mode_index;    // from the live SMBus settings (0xFF = none)
    uint8_t  confirmed_mode_index;  // last *applied* BIOS mode_index (0xFF = none)
    bool     bios_widescreen;       // the active/pending mode dword's WIDESCREEN bit (bit 28)

    // --- context ---
    arb_aspect_t user_aspect;   // user_config aspect override
    uint8_t  encoder_id;        // ENCODER_XCALIBUR / _CONEXANT / _FOCUS / _UNKNOWN
    bool     first_lock;        // this call is the cold-boot pll_settling first-lock resolve
    uint32_t now_ms;            // HAL_GetTick()

    // --- Xbox EEPROM (captured off the console's own boot-time read, smbus_probe.c) ---
    bool     eeprom_ws_valid;    // VideoSettings actually captured this boot
    bool     eeprom_widescreen;  // its Widescreen bit, meaningless unless eeprom_ws_valid
    bool     eeprom_std_valid;   // VideoStandard actually captured this boot
    bool     eeprom_std_is_pal;  // VideoStandard is PAL-I or PAL-M; meaningless unless
                                 // eeprom_std_valid. The console's factory region -- ground
                                 // truth for NTSC vs PAL.
    bool     eeprom_refresh_60hz; // xbox_eeprom_resolve_refresh() == 60Hz: a PAL-I console set
                                  // to 60 Hz runs NTSC-shaped rasters, so arb_region() calls it
                                  // NTSC. Meaningless unless eeprom_std_valid.

    // --- Live encoder aspect, all three encoders: Focus WSS_DATAF1, Conexant WSS, Xcalibur
    // register 0x07 (smbus_probe.c's xbox_encoder_get_wss_widescreen()). Preferred over
    // eeprom_widescreen in arb_resolve_aspect() when valid: unlike the EEPROM bit (captured
    // once at boot), this is rewritten live per mode/title. ---
    bool     enc_ws_valid;         // aspect captured and recognised this boot
    bool     enc_widescreen;       // its decoded aspect; meaningless unless enc_ws_valid

    // --- Xcalibur: the mode index its raster registers give (smbus_probe.c's
    // xbox_xcal_mode_index()), 0 before the first mode set (never a real index, and the
    // memset(0) default). Xcalibur runs several modes at the same ~27.000 MHz
    // (0x02/0x05/0x06/0x08/0x12), so this breaks the ties the clock can't. Its 0x0F/0x10/0x11
    // are register-identical to 0x02/0x04/0x08 and read as those. Filled by the caller; this
    // file stays pure.
    uint8_t  xcal_mode_index;
    // The encoder's sniffed width register and its write epoch (bumped on every write, so
    // every mode set -- aspect-only ones too): Conexant H_ACTIVE (0x78), Focus IHO (0x00,
    // sharing IHW's epoch). 0/0 on Xcalibur.
    uint16_t width_reg;
    uint8_t  width_reg_epoch;
} arb_snapshot_t;

typedef struct {
    // debounce accumulators, owned by the caller, zero-initialised
    uint32_t hw_incompat_since;   // hw_vic has read incompatible-with-active for this long
    uint32_t width_mismatch_since;
    uint32_t path_a_since;
    uint32_t bios_pending_since;
    uint32_t blind_since;
    uint32_t bios_stale_since;    // confirmed BIOS mode has disagreed with the live hw VIC
                                  // family this long, no packet pending -> hand back to L4
    bool     path_a_latched;      // Path A applied its VIC (2 or 3) and holds until the signal leaves
    uint8_t  pal50_sd_cand;      // last arb_pal50_sd_vic() reading (debounce candidate)
    uint32_t pal50_sd_since;     // when pal50_sd_cand was first seen -- must hold before trusted
    bool     hd_ever_seen;        // a 720p/1080i VIC (hw, active, or a BIOS HD mode) has been
                                  // seen at least once this boot. NOT proof of NTSC: a PAL
                                  // console with HD enabled outputs 720p/1080i too. arb_region()
                                  // uses it only when neither EEPROM nor pal_ever_seen answers.
    bool     pal_ever_seen;       // a BIOS PAL mode_index, a live 576-family hw_vic, or
                                  // (Conexant) a pclk in its 576p50 band -- the live readings on
                                  // a stable SD-range clock -- has been seen at least once this
                                  // boot. An NTSC console never produces a 576-line raster, so
                                  // this outranks hd_ever_seen. Lets arb_region() commit to PAL50
                                  // without EEPROM VideoStandard, which a board-only reset (no
                                  // console reboot) never re-captures; EEPROM still wins.
    // hw_stale (decide_target_vic()): the clock when hw_vic's raw byte last changed, and how
    // long the live clock has stayed far from it.
    uint8_t  hw_vic_locked;
    uint32_t hw_vic_lock_pclk;
    uint8_t  hw_fam_locked;     // arb_family_t of hw_vic when its family last changed
    uint32_t hw_fam_lock_pclk;  // pclk at that moment (see L1's staleness case (1))
    uint32_t hw_vic_drift_since;
    // How long L6's hold_480/hold_576 (decide_target_vic()) has continuously held while
    // waiting on a pal50_dbnc verdict that may never come -- see ARB_HOLD_STUCK_TIMEOUT_MS.
    uint32_t hold_stuck_since;
    // True while the hold_480/hold_576 stuck escape (ARB_HOLD_STUCK_TIMEOUT_MS) is what's
    // releasing the hold. The escape only fires when the PAL50 verdict never resolves, so
    // each firing is a bug to find -- stand_alone_loop() logs its rising edge.
    bool     hold_stuck_escaped;
    // Early-aspect skip (see decide_target_vic()): the raster signal at the arbiter's last
    // apply, when the early signal (a new Xcalibur index / Conexant or Focus WSS change) last
    // arrived, and the width register's epoch at that WSS change.
    uint8_t  xcal_idx_at_apply;
    uint8_t  xcal_idx_last;
    uint16_t width_reg_at_apply;
    bool     wss_seen;
    bool     wss_last;
    uint8_t  width_epoch_at_wss;
    uint32_t early_aspect_since;
    // L6 blind-fallback latch, like path_a_latched: the fallback verdict is recomputed from
    // the clock every call, and our own TMDS cycle zeroes the clock mid-apply, which without a
    // latch flipped the verdict every iteration (VIC 1/2/3 bouncing while blind). Frozen while
    // tmds_cycle_busy or pclk_hz == 0; reset when the detector stops being blind, so a new
    // blind episode doesn't inherit an old verdict.
    bool     blind_fb_valid;      // a verdict has been committed at least once this episode
    uint8_t  blind_fb_vic;        // the committed verdict
    bool     blind_fb_ws;
    bool     blind_fb_ana;
    uint8_t  blind_fb_cand_vic;   // in-progress candidate awaiting ARB_BLIND_FALLBACK_HOLD_MS
    bool     blind_fb_cand_ws;
    bool     blind_fb_cand_ana;
    uint32_t blind_fb_cand_since;
} arb_state_t;

typedef struct {
    arb_source_t source;
    bool         apply;              // false -> HOLD (nothing to do this iteration)
    uint8_t      target_vic;
    bool         widescreen;         // AVI aspect 16:9
    bool         anamorphic;         // 4:3 geometry + 16:9 AVI (640-wide widescreen want)
} vic_decision_t;

// Debounce/hold thresholds (ms).
#define ARB_HW_DEBOUNCE_MS        30
#define ARB_WIDTH_HOLD_MS         200
#define ARB_PATH_A_HOLD_MS        600
#define ARB_BIOS_WAIT_MS          2000
// Early-aspect skip bound: above the longest early-signal -> real-apply gap logged (~0.8s),
// so a misread raster signal can delay an aspect change by at most this, never block it.
#define ARB_EARLY_ASPECT_MAX_MS   1000
// Conexant mode-set burst: WSS (0x60-0x64) is written ~10ms before H_ACTIVE (0x78); wait at
// most this for the burst's H_ACTIVE write before treating a WSS change as aspect-only.
#define ARB_CX_BURST_WAIT_MS      50
#define ARB_BLIND_TIMEOUT_MS      60
#define ARB_BIOS_STALE_MS         250   // how long the live hw VIC must contradict a
                                        // confirmed BIOS mode before standalone reclaims
#define ARB_PAL50_SD_HOLD_MS      250   // a PAL50 pixel-clock SD-mode verdict must hold this
                                        // long (and no gate cycle in flight) before it can
                                        // drive a mode switch -- anti-flap near a split point
#define ARB_HOLD_STUCK_TIMEOUT_MS 1500  // L6's hold_480/hold_576 give up waiting on an
                                        // unresolved PAL50 verdict after this -- well past the
                                        // ~650 ms normal settle (400 ms pclk_stable + 250 ms
                                        // ARB_PAL50_SD_HOLD_MS)
#define ARB_BLIND_FALLBACK_HOLD_MS 300 // L6's blind-fallback verdict must hold this long
                                        // (frozen samples excluded) before it's trusted --
                                        // the same anti-flap role as ARB_PATH_A_HOLD_MS and
                                        // ARB_PAL50_SD_HOLD_MS

// hw_stale drift detection (decide_target_vic()), all encoders. Sized from Focus, whose PLL
// holds within ~1-2 kHz once locked: 25-50x that, still far below the gap between real SD
// modes. Not separately measured on Conexant/Xcalibur.
#define ARB_HW_DRIFT_HZ      50000UL   // sustained departure from the locked baseline
                                       // this large -> hw_vic is presumed stale
#define ARB_HW_DRIFT_HOLD_MS 300UL     // how long the departure must persist first

// Path A pixel-clock window: the PAL-I 480-line hybrid raster (Xcalibur 23.4 MHz, 50 Hz),
// between the PAL50 SD range and NTSC. Bounds first measured on Focus. Path A itself (L3)
// runs on Xcalibur only; the window also feeds the Focus/Xcalibur stale-576 checks.
#define ARB_PATH_A_PCLK_LOW_HZ    22500000UL   // Focus-measured.
#define ARB_PATH_A_PCLK_HIGH_HZ   23950000UL   // Focus-measured.

// PAL50 SD floor -- below this is 576i / Path A / noise, never a genuine SD raster. Also
// used by xbox_video_standalone.c's width sampling and xbox_video_timing.c's hybrid-row pick.
#define ARB_PAL50_SD_PCLK_LOW_HZ  24000000UL   // Focus-measured.

// Focus: tells the SDTV-family SD raster (mode_index 0x01/0x02, ~29.455 MHz) from the
// progressive 480p family (0x07/0x08, ~27.000 MHz) by clock alone -- width doesn't move the
// clock on Focus, only the family does (all four width x family combinations separate
// cleanly). Not known to hold for Conexant/Xcalibur. Also used by vic_arbiter.c: a 576-family
// reading at an SDTV-band clock contradicts itself (that band is interlaced 480-line).
#define ARB_FOCUS_PROGRESSIVE_PCLK_MAX_HZ 27500000UL  // Focus-measured. <= this: 480p-family
#define ARB_FOCUS_SDTV_PCLK_MIN_HZ        28900000UL  // Focus-measured. >= this: SDTV-family

// Conexant's two narrow-576 clocks: 640x576 0x05 at 24.625 MHz and 640x576 PAR 0x12 at
// 27.125 MHz (every Conexant capture, +/-1 kHz). Bands as low + span, kept clear of the
// neighbouring 480-line clocks (24.420 / 24.937, 26.875 / 27.000 / 27.378 MHz).
#define ARB_CX_640x576_PCLK_LOW_HZ       24550000UL
#define ARB_CX_640x576_PCLK_SPAN_HZ        150000UL
#define ARB_CX_640x576_PAR_PCLK_LOW_HZ   27100000UL
#define ARB_CX_640x576_PAR_PCLK_SPAN_HZ     50000UL
// Range test by unsigned subtraction: p - low <= span <=> low <= p <= low + span.
static inline bool arb_cx_clock_is_640x576(uint32_t p) {
    return p - ARB_CX_640x576_PCLK_LOW_HZ <= ARB_CX_640x576_PCLK_SPAN_HZ;
}
static inline bool arb_cx_clock_is_640x576_par(uint32_t p) {
    return p - ARB_CX_640x576_PAR_PCLK_LOW_HZ <= ARB_CX_640x576_PAR_PCLK_SPAN_HZ;
}

// A stable pixel clock at/above this is unambiguously HD (720p/1080i are ~74.25MHz;
// every SD mode tops out ~27MHz). The SD-only levels (width re-check, blind fallback)
// stand down above it -- otherwise a detector that blinks blind during an HD<->HD
// raster change (pclk never drops, since 720p and 1080i share a clock) gets an SD VIC.
#define ARB_HD_PCLK_MIN_HZ        40000000UL

// Any SD raster's clock: a blind detector reading with a stable clock in this range is
// plausibly an SD mode (a pending BIOS mode's check, the supervisor's relock settle).
#define ARB_SD_PCLK_MIN_HZ        12000000UL
#define ARB_SD_PCLK_MAX_HZ        30000000UL

vic_decision_t decide_target_vic(const arb_snapshot_t *in, arb_state_t *st);
// True when the clock (known-PAL50 console) or a fresh confirmed width (otherwise) puts the
// signal in a different SD family (640 / 720x480 / 576) than active_vic -- a mode change in
// progress, not a clock shift within the active VIC. stand_alone_loop()'s drift re-applies
// use it so they don't re-push the old VIC.
bool arb_clock_contradicts_active_family(const arb_snapshot_t *in, const arb_state_t *st);

#endif // __VIC_ARBITER_H__
