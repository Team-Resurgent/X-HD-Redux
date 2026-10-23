#ifndef __ADV7511_XBOX_H__
#define __ADV7511_XBOX_H__

// XBOX / TR HDMI specific ADV functions

#include "adv7511_i2c.h"
#include "adv7511.h"
#include "xbox_video_standalone.h"
#include "xbox_video_timing.h"

void adv7511_set_color_space(adv_color_space cs);
adv_color_space adv7511_get_color_space(void);
adv_color_space adv7511_get_effective_color_space(void);
const char* adv7511_get_color_space_name(void);

void adv7511_apply_fixed_calibration(void);
void adv7511_set_video_input_format(void);
void adv7511_init_spd_infoframe(void);
void init_adv(void);
void adv7511_reinit_chip(adv7511 *encoder, const xbox_encoder xb_encoder);
void init_adv_encoder_specific(const xbox_encoder xb_encoder);
void init_adv_audio();
void adv7511_mute_audio(void);
void adv7511_unmute_audio(void);
// SMBus WRITE_AUDIO_MUTE: hold the mute through the firmware's own unmutes, or release it.
void adv7511_set_audio_mute_hold(bool hold);
bool adv7511_audio_mute_held(void);
// TMDS mode-apply bracket: cycle_begin() gates output down (and captures the quiet-hold
// duration internally), then the caller programs the new timing, then cycle_end() starts
// the non-blocking hold+relock sequence. adv7511_tmds_cycle_service() must be called once
// per main-loop iteration, unconditionally, to advance and complete it.
// adv7511_tmds_cycle_busy() lets a caller check before starting a new cycle -- callers
// must not call cycle_begin()/_end() again while a previous cycle is still in flight.
void adv7511_tmds_cycle_begin(void);
// Same bracket, but the gate only closes if the apply inside it actually changes the
// raster -- an aspect-only change is applied in place with no blackout. See the .c.
void adv7511_tmds_cycle_begin_deferred(void);
void adv7511_tmds_cycle_end(void);
void adv7511_tmds_cycle_service(void);
bool adv7511_tmds_cycle_busy(void);
// Drivers up + audio unmute after AUDIO_WAIT, for output left down with no cycle to follow.
void adv7511_tmds_resume_output(void);
// WIRE_VIC_HONEST: once per main-loop iteration -- re-derives the wire VIC (0x3C / AVI DB4)
// from the applied timing and the settled pixel clock and updates it in place if it changed.
void adv7511_wire_vic_service(void);
// Writes 0x3B (pixel-repeat mode) for user_config's wire_vic_mode; callers re-apply the mode.
void adv7511_apply_wire_vic_mode(void);
// The internal VIC of the applied mode (what the arbiter tracks as active_vic) -- NOT the
// wire VIC. VIC_00 when nothing is applied or the chip's registers were reset since.
uint8_t adv7511_read_active_vic(void);
// The VIC actually transmitted: s_wire_vic in honest mode, the chip's 0x3D in auto mode.
uint8_t adv7511_wire_vic(void);
// The timing and AVI widescreen flag last programmed; false if nothing applied since the
// last chip (re)init.
bool adv7511_applied_mode(XboxVideoTiming *timing, bool *widescreen);
// The colour space that apply programmed (1 RGB full, 2 RGB limited, 3 YCbCr passthrough,
// 4 RGB passthrough) -- the wire format, not the setting or a fresh Auto resolution.
adv_color_space adv7511_applied_color_space(void);
// The AVI aspect an apply actually sends: the caller's widescreen request OR the VIC's own
// 16:9-ness (VIC 4/5 have no 4:3 form, likewise 3/18/22).
bool timing_ws_infoframe(const XboxVideoTiming * const timing, const bool widescreen);
// Re-programs the applied mode in place (HDMI/DVI mode, CSC, AVI) after the sink's EDID
// changed -- for a mode applied while output is still held off, no new TMDS cycle needed.
void adv7511_refresh_sink_config(void);
// True while a TMDS cycle has the mode programmed but output still off (waiting on PLL lock
// or edid_output_hold()).
bool adv7511_output_held_for_edid(void);
// Prints the host-parsed "Actual VIC Sent :" line with adv7511_wire_vic().
void adv7511_log_vic_sent(void);
void update_avi_infoframe(const bool widescreen, const uint8_t vic);
void adv7511_set_de_geometry(uint16_t hs_delay, uint16_t vs_delay, uint16_t h_active, uint16_t v_active, uint8_t interlaced_offset);
void adv7511_set_sync_adjust(bool enable, uint16_t hs_placement, uint16_t hs_duration, uint16_t vs_placement, uint16_t vs_duration);
// widescreen: the AVI aspect want, including the anamorphic 640-wide 16:9 case (callers
// pass `widescreen || anamorphic`). timing->vic always stays the true raster VIC -- it's
// written to 0x3C and read back as active_vic, which the arbiter compares its target
// against, so it must never be clamped to VIC_00.
void adv7511_apply_video_timing(const XboxVideoTiming * const timing, const bool widescreen, const bool from_bios);
bool adv7511_active_mode_is_bios(void);
// Narrow exception to adv7511_apply_video_timing() being the single choke point that sets
// this -- see its own comment in the .c. Only for a caller that's independently confirmed
// hardware doesn't need to change, just the logical ownership.
void adv7511_set_active_mode_is_bios(bool is_bios);
// False until the first real mode apply this boot -- see g_any_mode_applied in the .c.
bool adv7511_any_mode_applied(void);
// True if applying `timing`/`widescreen` would program anything different from what's
// already on the chip -- lets a caller skip the whole TMDS gate-cycle bracket (a 120-600ms
// black screen) for a re-apply that would change nothing. See s_applied_timing in the .c
// for why this exists and what it compares. Callers that must reprogram regardless (PLL/
// hotplug recovery, encoder re-probe, a forced reapply) simply don't consult it.
bool adv7511_timing_would_change(const XboxVideoTiming * const timing, const bool widescreen);

#endif // __ADV7511_XBOX_H__
