// BIOS mode path: an SMBus mode packet from the console's X-HD-aware BIOS/dashboard names
// the exact mode (mode_index + aspect), so it is applied directly instead of being inferred
// from the signal. bios_loop() takes packets in; the arbiter decides when BIOS owns the
// picture; bios_reconcile_pending_target() applies.

#include "../shared/adv7511_vic.h"
#include "../shared/adv7511_xbox.h"
#include "../shared/debug.h"
#include "../shared/user_config.h"
#include "../shared/xbox_eeprom_video.h"
#include "../shared/xbox_mode_index.h"
#include "adv7511.h"
#include "xbox_video_bios.h"
#include "smbus_i2c.h"
#include "smbus_probe.h"

static uint8_t current_active_mode_index = 0x07;

// The last packet applied, so a repeat of it isn't reapplied.
static uint32_t last_applied_mode = 0;
static uint32_t last_applied_avinfo = 0;
static uint32_t last_applied_region = 0;
static bool last_applied_valid = false;

// A new packet waits to be applied.
static bool s_packet_pending = false;

// True while a packet waits to be applied -- standalone's blind-fallback guess holds off
// instead of racing it.
bool bios_reconcile_target_pending(void) {
    return s_packet_pending;
}

static bool bios_resolve_mode(const xbox_encoder xb_encoder, const uint32_t mode,
                              const uint32_t avinfo,
                              XboxVideoTiming * const timing, bool * const avi_widescreen);
static void bios_apply_resolved(const uint32_t mode, const uint8_t mode_index,
                                 const XboxVideoTiming * const timing, const bool avi_widescreen);

uint8_t get_active_bios_mode_index(void) {
    return current_active_mode_index;
}

// The last-received mode dword, for the arbiter snapshot (bit 28 = widescreen, bits 23:16 =
// mode_index): standalone reads the packet through the BIOS path, not the SMBus settings.
uint32_t bios_last_mode_dword(void) {
    return getSMBusSettings()->mode;
}

void reapply_active_bios_mode(const xbox_encoder xb_encoder) {
    const SMBusSettings * const vid_settings = getSMBusSettings();
    if (vid_settings->mode != 0) {
        set_video_mode_bios(xb_encoder, vid_settings->mode, vid_settings->avinfo);
    }
}

// Applies the pending packet. Only an applier: the arbiter (decide_target_vic()) decides
// whether BIOS owns the picture. Runs every main-loop pass; a no-op unless a packet waits.
void bios_reconcile_pending_target(const xbox_encoder xb_encoder) {
    const SMBusSettings * const vid_settings = getSMBusSettings();
    // s_packet_pending is set only when a NEW packet arrives, so an applied packet can't fight
    // a later standalone apply. If the dashboard keeps sending, its next packet re-asserts
    // BIOS; if it went quiet, standalone drives.
    if (vid_settings->mode == 0 || !s_packet_pending) {
        return;
    }

    // Already applied and still on screen.
    if (last_applied_valid && adv7511_active_mode_is_bios() &&
        vid_settings->mode == last_applied_mode &&
        vid_settings->avinfo == last_applied_avinfo &&
        vid_settings->region == last_applied_region) {
        s_packet_pending = false;
        return;
    }

    // A TMDS cycle's tail is still running -- retry next pass.
    if (adv7511_tmds_cycle_busy()) {
        return;
    }

    const uint8_t mode_index = xbox_mode_index(vid_settings->mode);
    XboxVideoTiming want;
    bool want_ws;
    if (!bios_resolve_mode(xb_encoder, vid_settings->mode, vid_settings->avinfo,
                           &want, &want_ws)) {
        debug_log("[BIOS] bad idx %02X\r\n", (unsigned)mode_index);
        s_packet_pending = false;
        return;
    }

    last_applied_mode = vid_settings->mode;
    last_applied_avinfo = vid_settings->avinfo;
    last_applied_region = vid_settings->region;
    last_applied_valid = true;

    // The chip already holds exactly this timing: take ownership without reprogramming.
    // Routine -- different mode_index values share a raster (0x07/0x08/0x11 on Focus), and
    // the first packet of a boot usually matches what standalone already applied. A full
    // apply would cost a TMDS cycle and an audio mute for nothing.
    if (!adv7511_timing_would_change(&want, want_ws)) {
        adv7511_set_active_mode_is_bios(true);
        current_active_mode_index = mode_index;
        s_packet_pending = false;
        // Host tools track ownership changes through this line; the wire VIC is unchanged.
        adv7511_log_vic_sent();
        return;
    }

    // Deferred cycle: an aspect-only change on the same raster (every 4:3/16:9 pair) applies
    // in place, with no blackout.
    adv7511_tmds_cycle_begin_deferred();
    bios_apply_resolved(vid_settings->mode, mode_index, &want, want_ws);
    adv7511_tmds_cycle_end();
    s_packet_pending = false;
}

// Reapplies the active mode (BIOS or standalone) with audio muted around it. In place: no
// TMDS cycle, and the input gate is left as it is (an HPD disconnect or a running cycle may
// have it closed).
void reapply_active_mode(const xbox_encoder xb_encoder) {
    adv7511_mute_audio();
    if (adv7511_active_mode_is_bios()) {
        reapply_active_bios_mode(xb_encoder);
    } else {
        reapply_active_video_mode(xb_encoder);
    }
    adv7511_unmute_audio();
}

// Main-loop intake: takes a new packet (and the SMBus control writes the ISR stashed) and
// marks it pending; bios_reconcile_pending_target() applies it.
void bios_loop(xbox_encoder * xb_encoder, bool * encoder_mismatch) {
    if (video_mode_updated()) {
        // Copy the packet and its sequence number first: a newer packet can land mid-way, and
        // ack_video_mode_update() then leaves it pending instead of dropping it.
        const uint32_t seq = get_video_mode_update_sequence();
        const SMBusSettings vid_settings = *getSMBusSettings();
        // (No intake log line: the ISR's "SMBus: encoder=... mode=..." already records every
        // applied packet, and the GUI keys its packet event off that.)

        // Something for the Chief -- fires once per launch (on title-ID change).
        static uint32_t s_last_titleid = 0;
        if (vid_settings.titleid != s_last_titleid) {
            s_last_titleid = vid_settings.titleid;
            if (vid_settings.titleid == 0x4D530004u || vid_settings.titleid == 0x4D530064u) {
                debug_log("[BIOS] By the rings...\r\n");
            }
        }

        // A garbage encoder byte (dashboard bug, bit flip) would select the wrong DDR bus
        // order and garble the picture -- ignore it. A valid one is ground truth (the BIOS
        // talks to the encoder directly) and wins over the passive address probe.
        const bool encoder_valid = (vid_settings.encoder == ENCODER_CONEXANT ||
                                    vid_settings.encoder == ENCODER_FOCUS ||
                                    vid_settings.encoder == ENCODER_XCALIBUR);

        // Latch it and stop the probe; cheap and idempotent, so done on every packet.
        if (encoder_valid) {
            smbus_force_encoder(vid_settings.encoder);
        }

        bool encoder_changed = encoder_valid && (*xb_encoder != vid_settings.encoder);
        if (encoder_changed) {
            smbus_latch_encoder(vid_settings.encoder, xb_encoder, encoder_mismatch);
            // That setup changed registers the timing depends on: reapply even if the packet
            // repeats the last one.
            last_applied_valid = false;
        }

        s_packet_pending = true;

        ack_video_mode_update(seq);
    }

    // SMBus colour-space writes have their own pending flag (see smbus_i2c.h).
    if (consume_color_space_reapply_pending()) {
        reapply_active_mode(*xb_encoder);
    }

    // SMBus control-suite writes (141-145, 152-154) the ISR stashed: register, flash and
    // reapply work happens here, outside the ISR.
    smbus_consume_settings_changes(*xb_encoder);

    // WRITE_RAM_APPLY's flash page write, likewise deferred out of the I2C2 ISR.
    smbus_consume_flash_write();
}

// Resolves a mode dword to the timing and AVI aspect it would apply, with no side effects on
// the chip. The single place the aspect is decided -- the reconcile path asks "would this
// change anything?" with it, and the apply uses the same answer (two separate derivations
// drifted apart once: the 0x0F and 0x07 width bugs). False for an unsupported mode_index.
static bool bios_resolve_mode(const xbox_encoder xb_encoder, const uint32_t mode,
                              const uint32_t avinfo,
                              XboxVideoTiming * const timing, bool * const avi_widescreen) {
    const uint8_t mode_index = xbox_mode_index(mode);
    bool widescreen = (mode & XBOX_VIDEO_MODE_BIT_WIDESCREEN) != 0;
    const uint8_t m = xbox_mode_flags(mode_index, (uint8_t)xb_encoder);
    const mode_family_t fam = (mode_family_t)(m & MODE_FAM_MASK);
    const bool is_hd_idx = mode_family_is_hd(fam);

    // 480p (0x07/0x08/0x11): the packet's widescreen bit is not reliable here -- the console
    // sends the identical packet for 4:3 and 16:9 (only Direct3D's present flags differ, and
    // those never reach SMBus). When the bit is clear, fall back to avinfo bit 16 (set on
    // some consoles, e.g. Focus), then to the EEPROM Widescreen bit -- the same last resort
    // standalone uses, so both paths resolve the same signal the same way.
    if ((m & MODE_480P) && !widescreen) {
        if (avinfo != 0) {
            widescreen = (avinfo & XBOX_VIDEO_AVINFO_BIT_WIDESCREEN) != 0;
        } else {
            uint32_t vset;
            if (xbox_eeprom_get_video_settings(&vset)) {
                widescreen = xbox_eeprom_decode_widescreen(vset);
            }
        }
    }

    // 640-wide 480-line rasters: NTSC 0x01/0x0F, PAL 0x03/0x10, and 0x07 on Xcalibur only (on
    // Conexant/Focus 0x07 is 720-wide; xbox_get_video_timing() picks its row the same way).
    // CEA-861 has no 640-wide 16:9 VIC, so these keep the 4:3 VIC and 640 geometry and send a
    // 16:9 request anamorphically (AVI aspect bits only). 640x576 (0x05/0x12) is not here: it
    // has its own 4:3/16:9 VIC pair (21/22).
    const bool is_640_idx = (fam == FAM_SD_640);

    // The user's aspect setting (SD only -- HD is 16:9 by format) overrides the packet.
    const adv_aspect_ratio ar_cfg = user_config_get()->aspect_ratio;
    if (!is_hd_idx && ar_cfg != ASPECT_RATIO_AUTO) {
        widescreen = (ar_cfg == ASPECT_RATIO_16_9);
    }

    bool anamorphic_sd = false;
    if (is_640_idx) {
        anamorphic_sd = widescreen;
        widescreen = false;
    }

    if (!xbox_get_video_timing(xb_encoder, mode_index, widescreen, timing)) {
        return false;
    }
    // Anamorphic: the timing keeps its 4:3 VIC (1), which is also the internal VIC the arbiter
    // compares against; only the AVI aspect says 16:9.
    *avi_widescreen = widescreen || anamorphic_sd;
    return true;
}

// Programs a mode bios_resolve_mode() already resolved.
static void bios_apply_resolved(const uint32_t mode, const uint8_t mode_index,
                                 const XboxVideoTiming * const timing, const bool avi_widescreen) {
    current_active_mode_index = mode_index;

    // Same shape as standalone's [AUTODETECT] line plus the packet. p/i is the scan on HDMI
    // (only 1080i is interlaced); the aspect is the one actually sent (HD is forced 16:9);
    // "timing VIC" is the timing row's CEA family, not the wire VIC ("Actual VIC Sent :").
    const bool ws = timing_ws_infoframe(timing, avi_widescreen);
    debug_log("[BIOS] %08X idx=%02X %ux%u%c %s%s hs=%u timing VIC %u\r\n",
              mode, mode_index, timing->active_w, timing->active_h,
              timing->interlaced_offset ? 'i' : 'p', ws ? "16:9" : "4:3",
              (ws && timing->active_w == 640) ? " anamorphic" : "",
              timing->delay_hs, timing->vic);

    adv7511_apply_video_timing(timing, avi_widescreen, true);
}

// Resolves and applies a mode dword now, outside the reconcile path (reapplies, the `m` CLI
// command). False -- after logging "[BIOS] bad idx" -- for an unsupported mode_index, so the
// CLI doesn't report success when nothing was applied.
bool set_video_mode_bios(const xbox_encoder xb_encoder, const uint32_t mode, const uint32_t avinfo) {
    XboxVideoTiming timing;
    bool avi_widescreen;
    if (!bios_resolve_mode(xb_encoder, mode, avinfo, &timing, &avi_widescreen)) {
        debug_log("[BIOS] bad idx %02X\r\n", (unsigned)xbox_mode_index(mode));
        return false;
    }
    bios_apply_resolved(mode, xbox_mode_index(mode), &timing, avi_widescreen);
    return true;
}
