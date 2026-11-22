// SPDX-License-Identifier: GPL-3.0-only
#ifndef __XBOX_VIDEO_STANDALONE_H__
#define __XBOX_VIDEO_STANDALONE_H__

#include <stdint.h>
#include <stdbool.h>
#include "adv7511.h"
#include "adv7511_vic.h"
#include "types.h"

typedef enum {
    XBOX_VIDEO_480p_640,
    XBOX_VIDEO_480p_720,
    XBOX_VIDEO_720p,
    XBOX_VIDEO_1080i,
    // PAL mode_index 0x06 (720x576) -- the only PAL mode_index with a width-correct
    // progressive CEA VIC (17/18).
    XBOX_VIDEO_576p_720,
    // PAL mode_index 0x05 -- 640-wide, 576-line content. No CEA VIC matches this geometry;
    // broadcast under VIC 21/22 (576i, 2x-clock) as a placeholder.
    XBOX_VIDEO_576i_640
} video_modes_vic;

// pclk_hz must be read once per main-loop iteration by the caller and threaded through
// here (the clock can't change within one iteration, so one read serves every consumer).
//
// current_hw_vic (0x3E) and active_vic (adv7511_read_active_vic(), the internal VIC) can't
// be cached the same way: a gate cycle earlier in the pass can disturb 0x3E, and an apply
// changes the active VIC. Pass a value read just before the call.
bool stand_alone_loop(adv7511 * encoder, const xbox_encoder xb_encoder, uint8_t current_hw_vic, uint8_t active_vic, uint32_t pclk_hz, bool first_lock, bool force_reapply);
// Drops a queued standalone apply when a BIOS packet arrives.
void stand_alone_cancel_queued(adv7511 *encoder);


// The VIC -> standalone apply mapping used by stand_alone_loop(); public for
// run_standalone_fixtures.c. pclk_hz feeds VIC 0/1/2/3 (window check, and the
// Conexant standalone pal_i_50 NTSC/PAL split) and VIC 21/22 (Conexant's 27.10-27.15MHz
// narrow-576i PAR routing inside XBOX_VIDEO_576i_640) -- pass 0 if unknown.
void apply_standalone_by_vic(const xbox_encoder xb_encoder, uint8_t vic,
                              uint32_t pclk_hz, bool mailbox_widescreen);

void reapply_active_video_mode(const xbox_encoder xb_encoder);

// False while standalone has a mode decision under way: a mode change open or just closed
// (its verdict still forming), a decision wanted, or one the arbiter is still debouncing (a
// verdict pending, a blind-fallback candidate other than what's applied). As of the last
// stand_alone_loop().
bool stand_alone_settled(void);
// The mode-change half of stand_alone_settled() alone: no change open and its verdict grace
// past. What a BIOS-owned re-apply waits for -- the arbiter's standalone state is moot there
// (its BIOS decisions often want an apply the BIOS path makes).
bool stand_alone_change_settled(void);
// Nothing under way for whoever really owns the picture: a BIOS with a packet behind it
// (stand_alone_change_settled()), or else standalone (stand_alone_settled()) -- after a freeze
// the chip can still say BIOS with no packet left. What the wire-VIC service waits for.
bool stand_alone_owner_settled(void);
// The signal lost and coming back -- a PLL loss, or an HPD reconnect while BIOS owns the
// picture (standalone's own come in as force_reapply): a mode change from now (unsettled at
// once, even with stand_alone_loop() not running through the relock's settle wait), opened
// on its next pass.
void stand_alone_note_signal_loss(void);

// SD width, 640 (narrow) vs 720 (wide): the detector reads both as the same 480-line VIC.
// From each encoder's width signal (Focus IHO/IHW, Conexant H_ACTIVE, Xcalibur mode_index),
// else the pixel clock. sd_width_is_wide()/_is_confirmed() read the debounced result
// (confirmed: a debounced sample has landed this boot).
// One debounce step from the live width inputs. stand_alone_loop() runs it every iteration;
// the supervisor also runs it while the PLL settles, so first lock sees a confirmed width.
void sd_width_debounce_update(const xbox_encoder xb_encoder, uint32_t pclk_hz);
bool sd_width_is_wide(void);
bool sd_width_is_confirmed(void);
// Which input produced the last width sample, for the [ARB] line's wsrc=: "pclk",
// "focus-ihw", "cx-hact", "focus-ihw-stale-held" or "xcal-reg".
uint8_t sd_width_last_source(void);
// True when the last sample was a stale-held carryover (source 3 above) rather than a
// freshly-classified reading -- see vic_arbiter.h's arb_snapshot_t.width_stale_held.
bool sd_width_is_ihw_stale_held(void);
// True when the last sample came from the Xcalibur's mode index and that index predates the
// console's latest mode packet -- see arb_snapshot_t.width_predates_bios.
bool sd_width_predates_bios(void);

#endif // __XBOX_VIDEO_STANDALONE_H__
