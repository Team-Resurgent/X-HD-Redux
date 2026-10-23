// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT
//
// One iteration of the application main loop: HPD/MSEN and PLL-lock supervision, EDID retry,
// re-apply latches, the post-relock settle wait, the standalone/BIOS dispatch and the output
// restore catch-all. main.c keeps the one-time hardware init; this file is what the host
// tests (run_supervisor_fixtures.c) drive.

#include "stm32f0xx_hal.h"
#include "../shared/adv7511_i2c.h"
#include "../shared/adv7511.h"
#include "../shared/adv7511_xbox.h"
#include "../shared/debug.h"
#include "../shared/xbox_video_standalone.h"
#include "../shared/vic_arbiter.h"
#include "../shared/gpio.h"
#include "../shared/defines.h"
#include "smbus_i2c.h"
#include "main.h"
#include "xbox_video_bios.h"
#include "../shared/adv7511_edid.h"
#include "../shared/user_config.h"
#include "video_supervisor.h"

static bool last_pll_locked = false;
static bool last_hpd = false;
static bool last_msen = false;

// True once adv7511_reinit_chip() has run this boot (the HPD+MSEN wait below). The HPD/MSEN
// edge handling only starts then -- there is nothing to disconnect from before.
static bool chip_configured = false;
// When HPD+MSEN were first seen together before configuration (0 = not currently both), so a
// brief glitch can't start the 20 ms debounce early.
static uint32_t hpd_msen_ready_since = 0;

// A BIOS reapply waiting for the TMDS cycle to be free. needs_reapply is a one-shot edge;
// without this latch a request arriving mid-cycle would be lost.
static bool pending_bios_reapply = false;
// Set alongside pending_bios_reapply by a PLL relock only: the chip kept its registers
// through the loss, so the reapply may take the deferred bracket (an unchanged raster is
// re-pushed in place with no gate cycle). Reconnects / encoder re-probes / EDID retries
// leave it clear -- those can change state (CSC, chip reset) the in-place path skips.
static bool pending_bios_reapply_relock = false;

// State for the non-blocking first-lock settle wait below.
static bool pll_settling = false;
static uint32_t pll_settle_start = 0;
static uint32_t pll_settle_next_poll = 0;
static uint8_t pll_settle_last_seen = 0xFF;
static uint8_t pll_settle_stable_count = 0;
static bool pll_settle_was_blocked = false;

// (Re)start the settle wait's full window: first poll now, no stable reading yet.
static void pll_settle_arm(void)
{
    pll_settle_start = HAL_GetTick();
    pll_settle_next_poll = pll_settle_start;
    pll_settle_last_seen = 0xFF;
    pll_settle_stable_count = 0;
}

// A PLL relock that will be followed by a reapply cycle leaves the TMDS drivers down
// instead of powering them up immediately: up-then-gated-again showed the sink two
// link drops for one event. Whatever applies next powers them up itself (the cycle's
// PLL_POLL stage); if nothing does -- a same-mode relock that resolves with no cycle,
// or an in-place apply -- the catch-all after the watchdog powers them up once.
static bool tmds_power_up_pending = false;

// PLL-loss debounce, encoder probe, recovery jumper and telemetry state.
#define PLL_LOSS_DEBOUNCE_MS 50
static uint32_t pll_loss_candidate_time = 0;
static bool s_probe_disarmed = false;
static int8_t   rec_prev = -1;
static uint32_t rec_edge_ms = 0;
static uint8_t  rec_idx = 2;
static uint8_t s_tlm_div = 0;
static bool s_tlm_prev_blind = false;

static void update_diagnostic_leds(bool pll_lock, bool bios_active, uint8_t sent_vic, bool encoder_mismatch, bool hdmi_connected, uint8_t line_vic, bool encoder_resolved, uint32_t pixel_clock_hz, bool smbus_activity_seen)
{
    uint32_t now = HAL_GetTick();

    // Encoder not resolved yet: both LEDs off, then both blinking together once it has taken
    // too long (15 s if an SMBus host was seen, else 1 s).
    if (!encoder_resolved) {
        static uint32_t unresolved_since = 0;
        if (unresolved_since == 0) {
            unresolved_since = now;
        }
        uint32_t elapsed = now - unresolved_since;
        uint32_t threshold = smbus_activity_seen ? 15000 : 1000;
        if (elapsed > threshold) {
            bool phase = ((now / 400) % 2) == 0;
            set_led_1(phase);
            set_led_2(phase);
        } else {
            set_led_1(false);
            set_led_2(false);
        }
        return;
    }

    // Encoder mismatch (the board just self-latched a different encoder than its saved one):
    // green/blue alternate for 2 s, overriding everything else.
    static uint32_t mismatch_since = 0;
    if (encoder_mismatch && mismatch_since == 0) {
        mismatch_since = now;
    }
    if (mismatch_since != 0 && (now - mismatch_since) < 2000) {
        bool phase = ((now / 300) % 2) == 0;
        set_led_1(phase);
        set_led_2(!phase);
        return;
    }

    // Signal panic: PLL locked and a display connected, but the detector blind with no pixel
    // clock for over a second -- a clock toggling with no real format behind it. The
    // zero-clock requirement excludes real signals the detector just can't classify. Not over
    // a BIOS-owned mode: the packet named the format, and the clock reading is the audio CTS,
    // 0 until the console's S/PDIF starts ~10 s into a boot -- a BIOS-applied PAL hybrid
    // (detector blind) panicked with a good picture until then.
    static uint32_t blind_since = 0;
    if (pll_lock && hdmi_connected && !bios_active && line_vic == VIC_00_VIC_Unavailable &&
        pixel_clock_hz == 0) {
        if (blind_since == 0) {
            blind_since = now;
        }
    } else {
        blind_since = 0;
    }
    if (blind_since != 0 && (now - blind_since) > 1000) {
        bool phase = ((now / 80) % 2) == 0;
        set_led_1(phase);
        set_led_2(!phase);
        return;
    }

    // LED 1 (Green): PLL Lock & HDMI Cable Status
    if (pll_lock && hdmi_connected) {
        set_led_1(true); // Solid ON: GPU Clock Locked + HDMI Cable Connected
    } else if (pll_lock && !hdmi_connected) {
        set_led_1((now / 100) % 2 == 0); // Fast blink (100ms): Clock Locked, No Display
    } else if (!pll_lock && hdmi_connected) {
        set_led_1((now / 500) % 2 == 0); // Slow blink (500ms): Display Connected, No GPU Clock
    } else {
        set_led_1(false);
    }

    // LED 2 (Blue): blinks out the active video category, keyed on the live sent VIC.
    // 1 blink  = 576p / 576i PAL SD (VIC 17, 18, 21, 22)
    // 2 blinks = 480p / VGA SD (VIC 1, 2, 3)
    // 3 blinks = 720p HD (VIC 4)
    // 4 blinks = 1080i HD (VIC 5)
    // (At most 4: five 100 ms blinks at standalone speed blur together.)
    uint8_t blinks;
    switch (sent_vic) {
        case VIC_01_VGA_640x480_4_3:
        case VIC_02_480p_60__4_3:
        case VIC_03_480p_60_16_9:
            blinks = 2; // 480p / VGA SD
            break;
        case VIC_04_720p_60_16_9:
            blinks = 3; // 720p HD
            break;
        case VIC_05_1080i_60_16_9:
            blinks = 4; // 1080i HD
            break;
        case VIC_17_576p_50__4_3:
        case VIC_18_576p_50_16_9:
        case VIC_21_576i_50_2xClk_4_3:
        case VIC_22_576i_50_2xClk_16_9:
            blinks = 1; // 576p / 576i PAL SD
            break;
        default:
            blinks = 2;
            break;
    }

    // BIOS: 2.1 s cycle (4 blinks max x 400 ms + 500 ms pause). Standalone: the same pattern
    // at double speed.
    uint32_t unit_ms = bios_active ? 400 : 200;
    uint32_t pause_ms = bios_active ? 500 : 250;
    uint32_t cycle_time = now % (4UL * unit_ms + pause_ms);
    uint32_t active_window = (uint32_t)blinks * unit_ms;

    if (cycle_time < active_window) {
        set_led_2((cycle_time % unit_ms) < (unit_ms / 2));
    } else {
        set_led_2(false);
    }
}

// SMC audio clamp mute: when it started; 0 = none pending.
static uint32_t s_clamp_muted_at = 0;

#ifdef XHD_HOST_TEST
// Host tests only: puts the loop state back to its boot values between sessions. On the
// board the C runtime already starts every static above at exactly these values.
void video_supervisor_init(void)
{
    s_clamp_muted_at = 0;
    last_pll_locked = false;
    last_hpd = false;
    last_msen = false;
    chip_configured = false;
    hpd_msen_ready_since = 0;
    pending_bios_reapply = false;
    pending_bios_reapply_relock = false;
    pll_settling = false;
    pll_settle_start = 0;
    pll_settle_next_poll = 0;
    pll_settle_last_seen = 0xFF;
    pll_settle_stable_count = 0;
    pll_settle_was_blocked = false;
    tmds_power_up_pending = false;
    pll_loss_candidate_time = 0;
    s_probe_disarmed = false;
    rec_prev = -1;
    rec_edge_ms = 0;
    rec_idx = 2;
    s_tlm_div = 0;
    s_tlm_prev_blind = false;
}
#endif

// Video info for SMBus opcodes 22/151 (and the refresh in the UART extras line): what was
// applied, what the console sends, and the exact refresh from the measured clock.
static void publish_video_info(bool pll_lock, bool bios_active, uint8_t line_vic, xbox_encoder xb_encoder_for_vi)
{
    XhdVideoInfo vi = {0};
    XboxVideoTiming t;
    bool ws;
    const bool applied = adv7511_applied_mode(&t, &ws);
    const uint32_t pclk = adv7511_pixel_clock_settled_hz();
    vi.flags = (applied ? XHD_VI_APPLIED : 0) | (pll_lock ? XHD_VI_PLL_LOCK : 0)
             | (bios_active ? XHD_VI_BIOS : 0) | (edid_is_hdmi_sink() ? XHD_VI_HDMI : 0);
    vi.pixel_clock_hz = pclk;
    vi.detected_vic = line_vic;
    vi.console_mode_index = bios_active ? get_active_bios_mode_index() : 0xFF;
    if (user_config_get()->wire_vic_mode == WIRE_VIC_AUTO) {
        vi.flags |= XHD_VI_WIRE_AUTO;
    }
    if (applied) {
        const bool interlaced = t.interlaced_offset != 0;
        vi.flags |= (interlaced ? XHD_VI_INTERLACED : 0) | (ws ? XHD_VI_WIDESCREEN : 0)
                  | ((ws && t.active_w == 640) ? XHD_VI_ANAMORPHIC : 0);
        vi.width = t.active_w;
        vi.height = interlaced ? (uint16_t)(t.active_h * 2u) : t.active_h;
        vi.internal_vic = t.vic;
        vi.wire_vic = adv7511_wire_vic();
        vi.color_space = (uint8_t)adv7511_applied_color_space();
        // The family comes from the applied mode, not the EEPROM region: a PAL console runs
        // PAL-60 modes (0x08xx19/1A/1B) on the NTSC rows at 59.94.
        vi.refresh_mhz = xbox_refresh_mhz(t.active_h, interlaced, pclk,
                                          xbox_profile_is_50hz(xbox_video_timing_last_profile_index()));
        int16_t off;
        if (xbox_position(xb_encoder_for_vi, true, &off, NULL, false)) {
            vi.pos_h = (int8_t)off;   // SMBus writes are limited to +-127
        }
        if (xbox_position(xb_encoder_for_vi, false, &off, NULL, false)) {
            vi.pos_v = (int8_t)off;
        }
    }
    smbus_publish_video_info(&vi);
}


// The debounced PLL lock (raw 0x9E[4]).
static bool debounced_pll_lock(bool raw_pll_lock)
{
    // A PLL loss must last PLL_LOSS_DEBOUNCE_MS before it is acted on (TMDS down, mute,
    // relock reapply). A glitched 0x9E read lasts under a millisecond, and a Conexant encoder
    // reset unlocks for ~10-15 ms on every mode change -- reacting only added our own
    // blackout. Real losses (reboot, cable) last seconds.
    // A lock reading counts at once; only a loss after an established lock is debounced.
    bool pll_lock;
    if (raw_pll_lock) {
        pll_loss_candidate_time = 0;
        pll_lock = true;
    } else if (!last_pll_locked) {
        // Not locked before (or already confirmed lost): nothing to debounce.
        pll_loss_candidate_time = 0;
        pll_lock = false;
    } else if (pll_loss_candidate_time == 0) {
        pll_loss_candidate_time = HAL_GetTick();
        pll_lock = true; // first false read after a genuine prior lock -- don't act on it yet
    } else {
        pll_lock = (HAL_GetTick() - pll_loss_candidate_time) < PLL_LOSS_DEBOUNCE_MS;
    }
    return pll_lock;
}

// HPD/MSEN: configure the chip once both are up, then handle disconnect/reconnect edges.
static void supervise_hdmi(adv7511 *enc, xbox_encoder xb_encoder, uint8_t reg42, bool hpd, bool msen,
                           bool *needs_reapply)
{
    if (!chip_configured) {
        // Configure the chip once HPD and Monitor Sense are both up (Programming Guide
        // 4.2.1: "wait for both Monitor Sense and HPD before powering up the chip") for
        // 20 ms. No timeout -- with HPD low every register write would be a no-op anyway.
        // Not a blocking wait: the rest of the loop keeps running.
        if (hpd && msen) {
            if (hpd_msen_ready_since == 0) {
                hpd_msen_ready_since = HAL_GetTick();
            } else if (HAL_GetTick() - hpd_msen_ready_since >= 20) {
                // No edid_update_cache() here: adv7511_reinit_chip() fills the cache
                // (through update_avi_infoframe() -> edid_is_hdmi_sink()), and a second
                // fetch would cost another blocking EDID read on every boot.
                adv7511_reinit_chip(enc, xb_encoder);
                smbus_note_chip_id(adv7511_read_register(0x00), adv7511_read_register(0xF5));
                chip_configured = true;
                // Seed the edge-detector below so it doesn't also treat this same
                // initial transition as a disconnect/reconnect/MSEN edge.
                last_hpd = hpd;
                last_msen = msen;
                debug_log("[HDMI] HPD+MSEN ready\r\n");
            }
        } else {
            hpd_msen_ready_since = 0;
        }
    } else {
        // HPD/MSEN edges after configuration: disconnect on HPD falling; recovery on HPD
        // rising, or on MSEN rising with HPD already high -- a sink can raise HPD (often
        // its 5 V rail) well before its receiver is ready (Programming Guide 4.2.1).
        // Recovery reapplies the mode through needs_reapply.
        if (hpd != last_hpd || msen != last_msen) {
            debug_log("[HDMI] HPD: %s, MSEN: %s (0x42=0x%02X)\r\n",
                      hpd ? "CONNECTED" : "DISCONNECTED",
                      msen ? "ACTIVE" : "INACTIVE",
                      reg42);
            if (!hpd && last_hpd) {
                // Cable disconnected: gate output and mute audio.
                adv7511_disable_video();   // also mutes audio
                debug_log("[HDMI] disconnected\r\n");
            } else if ((hpd && !last_hpd) || (msen && !last_msen && hpd)) {
                // Reconnected (HPD) or the sink became ready (MSEN). An HPD-low episode
                // resets the AVI InfoFrame, CSC and packet enables, and the sink may be a
                // different one (DVI/HDMI, aspect), so reopen the gate and reapply the
                // mode fully. Power up first: any HPD-low, even one too brief to sample,
                // puts the chip in Main Power Down, where every write is ignored.
                adv7511_ensure_powered_up();
                if (xb_encoder != ENCODER_UNKNOWN) {
                    adv7511_enable_video();
                    adv7511_unmute_audio();
                }
                // Refresh the EDID cache in case a different display was connected
                // while disconnected.
                edid_update_cache();
                // Resend the SPD InfoFrame (packet memory was reset; no-op on DVI).
                adv7511_init_spd_infoframe();
                *needs_reapply = true;
                debug_log("[HDMI] %s\r\n", (hpd && !last_hpd) ? "Reconnected"
                                                               : "Monitor Sense active -- reasserting");
            }
            last_hpd = hpd;
            last_msen = msen;
        }
    }
}

static void supervise_encoder_probe(void)
{
    // Encoder probe cutoff. The probe's interrupt (priority 2) sits below I2C2 (priority 1),
    // so it can stay armed safely; it disarms itself once resolved (an address match, or a
    // valid encoder byte in a BIOS packet). The caps only cover neither ever happening:
    //   - an SMBus host addressed us (0x69) but sent no valid encoder byte: 45 s;
    //   - no host at all (a BIOS without X-HD SMBus): the probe is the only way to identify
    //     the encoder, and some consoles address it rarely or late -- 180 s.
    if (!s_probe_disarmed) {
        // Already resolved: disarm now rather than at the cap.
        bool already_known = (smbus_get_probed_encoder_addr() != 0);
        uint32_t cap_ms = smbus_host_addressed_us() ? 45000u : 180000u;
        if (already_known || HAL_GetTick() >= cap_ms) {
            smbus_disarm_encoder_probe();
            s_probe_disarmed = true;
            if (already_known) {
                debug_log("[ENCODER PROBE] resolved\r\n");
            } else {
                debug_log("[ENCODER PROBE] %s cap, unknown\r\n",
                          smbus_host_addressed_us() ? "45s" : "180s");
            }
        }
    }
}

static void follow_encoder_format(xbox_encoder xb_encoder)
{
    // The encoder's input format (Conexant IN_MODE, Focus 0x92) is tracked all boot: a BIOS
    // or dashboard can switch it YCrCb -> RGB mid-boot, and a chainload can switch it back.
    // On a change, reapply so Auto re-resolves (RGB passthrough or not). A manual colour
    // setting wins.
    if (smbus_encfmt_take_dirty()) {
        bool in_rgb = false;
        xbox_encoder_input_is_rgb(&in_rgb, NULL);
        debug_log("[ENCFMT] encoder input -> %s\r\n", in_rgb ? "RGB" : "YCrCb");
        uint8_t ucs = user_config_get()->color_space;
        if ((ucs == COLOR_SPACE_AUTO || ucs == COLOR_SPACE_AUTO_SD_RGB_FULL) &&
            adv7511_any_mode_applied()) {
            reapply_active_mode(xb_encoder);
        }
    }
}

static void poll_recovery_jumper(void)
{
    // RECOVERY pin (PC15) grounded while running: step the encoder Xcalibur -> Conexant ->
    // Focus. A hardware-only way to set the encoder when the probe can't see it and the BIOS
    // sends no SMBus. One step per press (50 ms debounce), saved to page 63 via
    // smbus_force_encoder(), with the mismatch LED flash as feedback. The state at boot is
    // only adopted, so a jumper left installed (the bootloader's page-63 wipe) doesn't spin.
    uint8_t rec_now = recovery_jumper_enabled() ? 1 : 0;  // 1 = grounded
    if (rec_prev == -1) {
        rec_prev = (int8_t)rec_now;
    } else if (rec_now != rec_prev) {
        rec_prev = (int8_t)rec_now;
        rec_edge_ms = HAL_GetTick();
    } else if (rec_now == 1 && rec_edge_ms != 0 &&
               (HAL_GetTick() - rec_edge_ms) >= 50) {
        rec_edge_ms = 0;  // one action per press
        static const uint8_t enc_addr[3] = { ENCODER_XCALIBUR, ENCODER_CONEXANT, ENCODER_FOCUS };
        // Wrap by compare, not "% 3": a signed modulo here was the image's only use of
        // __aeabi_idivmod/__divsi3 (~470 B).
        if (++rec_idx >= 3) rec_idx = 0;
        smbus_force_encoder(enc_addr[rec_idx]);
        debug_log("[RECOVERY] encoder cycle -> %s\r\n", get_xbox_encoder_name(enc_addr[rec_idx]));
    }
}

static void publish_status(bool hpd, bool msen, bool pll_lock, bool bios_active, uint32_t pclk_hz,
                           uint8_t line_vic, xbox_encoder xb_encoder)
{
    // Telemetry (SMBus 15 / UART "tlm") and video info (SMBus 22): refreshed every 8th pass
    // (~30-40 ms; two extra I2C reads). Counters are bumped where events happen; the blind
    // edge check runs every pass so it can't miss one.
    if ((s_tlm_div++ & 0x07) == 0) {
        uint8_t tlm_flags = (hpd ? XHD_TLM_HPD : 0)
                          | (msen ? XHD_TLM_MSEN : 0)
                          // 0xD6[0]: 1 = input clock/video path GATED, 0 = passing.
                          | (!(adv7511_read_register(0xD6) & 0x01) ? XHD_TLM_VIDEO_UNGATED : 0)
                          | ((adv7511_read_register(0x44) & 0x20) ? XHD_TLM_AUDIO_ON : 0)
                          | (smbus_get_apply_frozen() ? XHD_TLM_FREEZE : 0)
                          | (pll_lock ? XHD_TLM_PLL_LOCK : 0);
        smbus_publish_telemetry(pclk_hz, line_vic, tlm_flags, (uint8_t)xb_encoder);
        publish_video_info(pll_lock, bios_active, line_vic, xb_encoder);
    }

    // Blind is an edge on the line detector (mode changes are counted in the apply).
    bool now_blind = pll_lock && (line_vic == VIC_00_VIC_Unavailable);
    if (now_blind && !s_tlm_prev_blind) smbus_telemetry_event(XHD_TLM_EV_BLIND);
    s_tlm_prev_blind = now_blind;
}

// Act on needs_reapply (reconnect, encoder correction) and the latched BIOS reapply.
static void handle_reapply(adv7511 *enc, xbox_encoder xb_encoder, bool needs_reapply, bool pll_lock,
                           uint8_t line_vic, uint8_t sent_vic, uint32_t pclk_hz)
{
    bool needs_reapply_started_cycle = false;
    if (needs_reapply) {
        // Reconnects and encoder corrections reapply through the same paths as a relock.
        if (adv7511_active_mode_is_bios()) {
            // Latch the request instead of acting on it directly -- see
            // pending_bios_reapply's declaration.
            pending_bios_reapply = true;
            pending_bios_reapply_relock = false;
        } else {
            // force_reapply: the arbiter still decides (a reconnect onto a changed signal
            // gets the right mode at once), and "no change" re-pushes the current mode to
            // restore the state the HPD-low wiped. line_vic/sent_vic are still current --
            // nothing since their read writes 0x3E or 0x3C. A started cycle owns un-gating.
            needs_reapply_started_cycle = stand_alone_loop(enc, xb_encoder, line_vic,
                                                           sent_vic, pclk_hz,
                                                           false /*first_lock*/, true /*force_reapply*/);
        }
    }

    // The latched BIOS reapply, retried every pass until the cycle is free -- and not during
    // the settle wait, whose polls need an undisturbed clock and detector. It is the only way
    // back to a picture after a relock with a quiet dashboard (no new packet to reconcile).
    if (pending_bios_reapply && !adv7511_tmds_cycle_busy() && !pll_settling) {
        pending_bios_reapply = false;
        if (pending_bios_reapply_relock) {
            adv7511_tmds_cycle_begin_deferred();
        } else {
            adv7511_tmds_cycle_begin();
        }
        pending_bios_reapply_relock = false;
        reapply_active_bios_mode(xb_encoder);
        adv7511_tmds_cycle_end();
        needs_reapply_started_cycle = adv7511_tmds_cycle_busy();
    }

    if (needs_reapply) {
        // Un-gate and unmute only with a lock and a known encoder, and no cycle starting or
        // running (a cycle un-gates itself). Unmuting here too covers an encoder that
        // resolved later this pass: the reconnect branch skipped both while it was unknown,
        // and the disconnect's mute would otherwise outlast the picture.
        if (!needs_reapply_started_cycle && !adv7511_tmds_cycle_busy() &&
            xb_encoder != ENCODER_UNKNOWN && pll_lock) {
            adv7511_enable_video();
            adv7511_unmute_audio();
        }
    }
}

static void supervise_pll(adv7511 *enc, xbox_encoder xb_encoder, bool pll_lock, uint32_t pclk_hz)
{
    // PLL relock and clock settle watchdog.
    //
    // Not while a TMDS cycle runs: the cycle unlocks 0x9E itself on purpose, reacting would
    // start a false "relock", and the cycle waits for the PLL itself. last_pll_locked stays
    // frozen until it finishes.
    if (adv7511_tmds_cycle_busy()) {
        // Note a settle wait blocked behind the cycle: its time budget must not run down
        // while it can't poll (re-armed below).
        if (pll_settling) {
            pll_settle_was_blocked = true;
        }
    } else if (pll_lock) {
        if (pll_settle_was_blocked && pll_settling) {
            // Blocked behind a cycle, which can eat the whole 1200 ms budget before one poll:
            // start a fresh window.
            pll_settle_was_blocked = false;
            pll_settle_arm();
        }
        if (!last_pll_locked) {
            last_pll_locked = true;
            debug_log("[PLL EVENT] GPU Pixel Clock LOCKED\r\n");

            // The TMDS drivers stay down (they went down on the loss): the settle wait and
            // usually a reapply cycle follow, and the cycle powers them up -- powering up
            // here too made the sink resync twice. With no cycle (same mode, or in place),
            // the catch-all after this watchdog powers them up. Not before the first real
            // mode: only the init placeholder is loaded then, and the first apply's cycle
            // powers them up.
            if (adv7511_any_mode_applied()) {
                tmds_power_up_pending = true;
            }

            // Clear transient interrupt flags and re-sync DE Generator to incoming clock
            adv7511_write_register(0x96, 0xFF);
            adv7511_update_register(0x17, 0b00000001, 0b00000000);
            adv7511_update_register(0x17, 0b00000001, 0b00000001);

            // Make sure the S/PDIF input (0x0B[7]) is on: the ACR, and so CTS and the pixel
            // clock reading, needs it -- with it off CTS reads 0 and every clock classifier
            // starves. Mutes leave it on; this guarantees it before the settle wait (CTS
            // re-acquires ~500 ms later), on the first lock of a boot too. Audio packets
            // (0x44[5]) stay muted until the output is back.
            adv7511_update_register(0x0B, 0b10000000, 0b10000000);

            // The outputs went down on the loss. A BIOS mode needs a reapply (latched, like a
            // reconnect) -- a relock with no new packet otherwise left the output dead.
            // Standalone recovers through the settle wait below.
            if (adv7511_active_mode_is_bios()) {
                // In place only if no full reapply (reconnect, re-probe) is already latched.
                if (!pending_bios_reapply) {
                    pending_bios_reapply_relock = true;
                }
                pending_bios_reapply = true;
            }

            // Kick off the non-blocking settle wait below; the first poll happens
            // next iteration.
            pll_settle_arm();
            pll_settling = true;
        } else if (pll_settling) {
            // Settle wait (non-blocking): two consecutive matching detector reads and a
            // settled clock, polled every 10 ms then 25 ms, capped at 1200 ms.
            //
            // The width debounce runs here too -- stand_alone_loop() doesn't run until this
            // resolves, and without it first lock saw no confirmed width (VIC 2, then VIC 1:
            // two cycles). It confirms within this wait, so it costs no time.
            sd_width_debounce_update(xb_encoder, pclk_hz);
            // A stable clock, not just a non-zero one: the first-lock decision adopts the
            // detector's family with no debounce, and the PAL50 clock cross-check that could
            // catch a wrong reading needs a stable clock. The detector settles faster than
            // the ACR (~500 ms).
            uint32_t now = HAL_GetTick();
            uint32_t elapsed = now - pll_settle_start;
            bool resolved = false;
            uint8_t measured_vic = VIC_00_VIC_Unavailable;

            if (elapsed >= 1200) {
                // Timed out: resolve with measured_vic still VIC 0.
                resolved = true;
            } else if ((int32_t)(now - pll_settle_next_poll) >= 0) {
                uint8_t v = adv7511_read_register(0x3E) >> 2;
                // A non-CEA raster reads VIC 0 forever, so a stable blind reading with a
                // plausible SD clock also counts as settled (the arbiter's blind-SD range)
                // -- otherwise every such relock took the full 1200 ms.
                bool valid_vic = (v != VIC_00_VIC_Unavailable);
                bool valid_blind_sd = (v == VIC_00_VIC_Unavailable &&
                                       pclk_hz >= ARB_SD_PCLK_MIN_HZ && pclk_hz <= ARB_SD_PCLK_MAX_HZ);
                if ((valid_vic || valid_blind_sd) && v == pll_settle_last_seen) {
                    if (pll_settle_stable_count < 2) {
                        pll_settle_stable_count++;
                    }
                    // Resolve once the detector is stable AND the clock has settled.
                    if (pll_settle_stable_count >= 2 && pclk_hz > 0 &&
                        adv7511_pixel_clock_stable()) {
                        measured_vic = v;
                        resolved = true;
                    }
                } else {
                    pll_settle_last_seen = v;
                    pll_settle_stable_count = 0;
                }
                pll_settle_next_poll = now + ((elapsed < 100) ? 10 : 25);
            }

            if (resolved) {
                pll_settling = false;

                // Hand the measured VIC to the arbiter as a first-lock snapshot: L1 keeps an
                // active BIOS mode, and L4 adopts the measured VIC without a debounce --
                // right for a relock, which isn't a real mode change. The cycle un-gates.
                stand_alone_loop(enc, xb_encoder, measured_vic,
                                 adv7511_read_active_vic(), pclk_hz,
                                 true /*first_lock*/, false /*force_reapply*/);
                // The loss muted audio and left the drivers down; a reapply cycle restores
                // both, and if none runs (same mode, or in place) the catch-all does.
            }
        } else {
            // The standalone pass, every iteration (BIOS packets apply below). Fresh reads:
            // a gate cycle earlier this pass can have disturbed 0x3E.
            uint8_t fresh_hw_vic = adv7511_read_register(0x3E) >> 2;
            uint8_t fresh_active_vic = adv7511_read_active_vic();
            stand_alone_loop(enc, xb_encoder, fresh_hw_vic, fresh_active_vic, pclk_hz, false, false);
        }

        // Apply a pending BIOS packet (the arbiter's L1 decides when it owns the picture).
        bios_reconcile_pending_target(xb_encoder);
    } else if (last_pll_locked && adv7511_any_mode_applied()) {
        // Hardware Reset on GPU Clock Drop / Reboot / Resolution Switch. Not before the first
        // real mode: the PLL can drop lock briefly ~20-90 ms after first acquiring it, and
        // nothing is on screen yet.
        last_pll_locked = false;
        smbus_telemetry_event(XHD_TLM_EV_PLL_LOSS);

        // Re-measure the clock's settle from scratch on the next acquisition.
        adv7511_pixel_clock_reset_stability();

        // 1. Cut TMDS line drivers immediately to prevent TV receiver jitter
        // (the next relock decides when they come back up).
        adv7511_power_down_tmds();
        tmds_power_up_pending = false;

        // 2. Hardware Audio Mute to prevent boot/mode pops
        adv7511_mute_audio();

        // 3. Clear transient hardware interrupt flags
        adv7511_write_register(0x96, 0xFF);

        debug_log("[PLL EVENT] GPU Pixel Clock LOST (9E=%02X)\r\n",
                  adv7511_read_register(0x9E));
    }
}

void video_supervisor_step(adv7511 *enc, xbox_encoder *xb_encoder_io, bool *encoder_mismatch_io)
{
    xbox_encoder xb_encoder = *xb_encoder_io;
    bool encoder_mismatch = *encoder_mismatch_io;

    debug_ring_flush();

    uint8_t reg9e = adv7511_read_register(0x9E);
    bool raw_pll_lock = (reg9e >> 4) & 0x01;

    // 0x42 read once per iteration -- HPD/MSEN for the disconnect handler below.
    uint8_t reg42 = adv7511_read_register(0x42);
    bool hpd = (reg42 >> 6) & 0x01;
    bool msen = (reg42 >> 5) & 0x01;

    bool pll_lock = debounced_pll_lock(raw_pll_lock);
    bool bios_active = adv7511_active_mode_is_bios();
    uint8_t sent_vic = adv7511_read_active_vic();

    bool needs_reapply = false;

    supervise_hdmi(enc, xb_encoder, reg42, hpd, msen, &needs_reapply);

    // The passively probed encoder (seen on the console's own SMBus traffic) self-latches like
    // a BIOS packet does; it sets needs_reapply if a mode already went out with the wrong
    // encoder's timing.
    smbus_check_probed_encoder(&xb_encoder, &encoder_mismatch, &needs_reapply);

    // EDID retry: a sink (AVR, HDMI switch) can hold HPD steady while its DDC EEPROM powers
    // up later, which no HPD/MSEN edge reports -- without this it stayed on the DVI fallback
    // with no audio until a replug. True only on the pass a retry succeeds.
    if (edid_retry_periodic()) {
        adv7511_init_spd_infoframe();
        // Output still held off for this read: reconfigure the applied mode in place and
        // let the cycle bring output up once. Output already up (as DVI): re-apply. No
        // mode applied yet: nothing to fix -- the first apply reads the new EDID (a forced
        // re-apply here would push a default mode ahead of the console's real one).
        if (adv7511_output_held_for_edid()) {
            adv7511_refresh_sink_config();
        } else if (adv7511_any_mode_applied()) {
            needs_reapply = true;
        }
        debug_log("[EDID] retry ok\r\n");
    }

    supervise_encoder_probe();
    follow_encoder_format(xb_encoder);
    // The console clamps its analog audio as a reboot/shutdown starts; HDMI audio isn't on
    // that clamp, so mute it here, for 1 s -- long enough for the audio buffer to run out.
    // A plain mute, not the SMBus hold: a mode apply unmutes as usual. Some BIOSes clamp
    // without resetting (PrometheOS loading Cerbios), so the mute must not outlast that.
    // At 1 s it unmutes only where the other unmute paths would: clock locked, encoder
    // known, no TMDS cycle, input not gated.
    const uint8_t smc_ev = smbus_smc_events_take();
    if (smc_ev & SMC_EV_AUDIO_CLAMP) {
        adv7511_mute_audio();
        s_clamp_muted_at = HAL_GetTick() | 1u;
        debug_log("[AUDIO] SMC clamp: muted\r\n");
    } else if (s_clamp_muted_at && (HAL_GetTick() - s_clamp_muted_at) >= 1000u) {
        s_clamp_muted_at = 0;
        if (pll_lock && hpd && xb_encoder != ENCODER_UNKNOWN && !adv7511_tmds_cycle_busy() &&
            !(adv7511_read_register(0xD6) & 0x01)) {
            adv7511_unmute_audio();
        }
    }
    // The console is shutting down (not rebooting): put the TV in standby too.
    if ((smc_ev & SMC_EV_SHUTDOWN) && !user_config_get()->cec_standby_off) {
        adv7511_cec_standby();
    }
    poll_recovery_jumper();

    uint8_t line_vic = adv7511_read_register(0x3E) >> 2;
    // Read once per iteration and threaded through the rest of the loop -- the clock
    // can't change within one iteration.
    uint32_t pclk_hz = adv7511_get_pixel_clock_hz();
    // Feed the clock stability tracker (no extra I2C): the classifiers wait on it through the
    // ~500 ms of CTS 0 after any disruption.
    adv7511_pixel_clock_note(pclk_hz);
    update_diagnostic_leds(pll_lock, bios_active, sent_vic, encoder_mismatch, hpd, line_vic, xb_encoder != ENCODER_UNKNOWN, pclk_hz, smbus_probe_activity_seen());

    // SMBus WRITE_SPD_APPLY committed a new SPD name in the ISR; resend the packet here.
    if (smbus_consume_spd_apply_pending()) {
        adv7511_init_spd_infoframe();
    }

    publish_status(hpd, msen, pll_lock, bios_active, pclk_hz, line_vic, xb_encoder);

    // Before adv_handle_interrupts(): it clears all of 0x97, and a CEC Tx completion sharing
    // the INT edge with another interrupt would be lost.
    cec_clock_out_follow_config();
    adv7511_cec_poll();
    // One Touch Play (page-63, on by default), once per boot from power-on,
    // HPD or not -- see adv7511_cec_one_touch_step(); it waits for the EDID itself.
    if (!user_config_get()->cec_one_touch_off) {
        adv7511_cec_one_touch_step(edid_cec_physical_address());
    }

    // Services the interrupt flag (clears/re-enables on 0x96).
    adv_handle_interrupts(enc);

    // Advances the non-blocking tmds-cycle state machine -- must run once per
    // iteration, unconditionally, before the busy-checks below.
    adv7511_tmds_cycle_service();

    handle_reapply(enc, xb_encoder, needs_reapply, pll_lock, line_vic, sent_vic, pclk_hz);

    // SMBus BIOS packets, every pass. bios_loop() only records the packet;
    // bios_reconcile_pending_target() (in supervise_pll()) applies it. Note the pending flag
    // first -- bios_loop() acks it. A new packet owns the next mode: drop any standalone
    // apply still queued.
    bool had_pending_mode_update = video_mode_updated();
    bios_loop(&xb_encoder, &encoder_mismatch);
    if (had_pending_mode_update) {
        stand_alone_cancel_queued(enc);
    }

    supervise_pll(enc, xb_encoder, pll_lock, pclk_hz);

    // Wire VIC follows the settled clock (see adv7511_wire_vic_service()) -- not while a
    // decided mode change is still queued (standalone's width wait): its apply re-judges it.
    if (!(enc->vic & ADV7511_VIC_CHANGED)) {
        adv7511_wire_vic_service();
    }

    // Catch-all for tmds_power_up_pending (see its declaration): once the relock has
    // settled and nothing else is queued or in flight, bring output back if no cycle
    // already did -- drivers up, then audio after the sink's lock time (the cycle's
    // own AUDIO_WAIT). Unmuting together with the power-up gave the sink none.
    if (tmds_power_up_pending && last_pll_locked && !pll_settling && !pending_bios_reapply &&
        !adv7511_tmds_cycle_busy() && !(enc->vic & ADV7511_VIC_CHANGED)) {
        tmds_power_up_pending = false;
        if (adv7511_tmds_powered_down()) {
            adv7511_tmds_resume_output();
        }
    }

    smbus_poll_sniffer();

    debug_cli_poll(xb_encoder);

    *xb_encoder_io = xb_encoder;
    *encoder_mismatch_io = encoder_mismatch;
}
