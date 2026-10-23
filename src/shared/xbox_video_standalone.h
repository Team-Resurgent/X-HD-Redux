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
// Drops a queued standalone apply (and its width wait) when a BIOS packet arrives.
void stand_alone_cancel_queued(adv7511 *encoder);


// The VIC -> standalone apply mapping used by stand_alone_loop(); public for
// run_standalone_fixtures.c. pclk_hz feeds VIC 0/1/2/3 (window check, and the
// Conexant standalone pal_i_50 NTSC/PAL split) and VIC 21/22 (Conexant's 27.10-27.15MHz
// narrow-576i PAR routing inside XBOX_VIDEO_576i_640) -- pass 0 if unknown.
void apply_standalone_by_vic(const xbox_encoder xb_encoder, uint8_t vic,
                              uint32_t pclk_hz, bool mailbox_widescreen);

void reapply_active_video_mode(const xbox_encoder xb_encoder);

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
const char *sd_width_last_source_str(void);
// True when the last sample was a stale-held carryover (source 3 above) rather than a
// freshly-classified reading -- see vic_arbiter.h's arb_snapshot_t.width_stale_held.
bool sd_width_is_ihw_stale_held(void);

#endif // __XBOX_VIDEO_STANDALONE_H__
