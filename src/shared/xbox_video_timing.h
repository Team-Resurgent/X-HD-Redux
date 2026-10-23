#ifndef __XBOX_VIDEO_TIMING_H__
#define __XBOX_VIDEO_TIMING_H__

#include <stdbool.h>
#include <stdint.h>
#include "types.h"

typedef struct {
    uint16_t delay_hs;
    uint8_t  delay_vs;
    uint16_t active_w;
    uint16_t active_h;
    uint8_t  interlaced_offset;
    bool     sync_adjust_enabled;
    uint16_t hsync_placement;
    uint16_t hsync_duration;
    uint16_t vsync_placement;
    uint16_t vsync_duration;
    uint8_t  vic;
} XboxVideoTiming;

// Derives exact video timing parameters for the ADV7511 from encoder, mode index (1 to 0x12,
// plus the standalone synthetics 0x13-0x15) and widescreen flag
bool xbox_get_video_timing(xbox_encoder encoder, uint8_t mode_index, bool widescreen, XboxVideoTiming *timing);

// Mode 0x04's (PAL 720x480 @ 50 Hz) base HS delay for `encoder`, before calibration -- the
// standalone Path A apply uses it.
uint16_t xbox_pal_720x480_hs_delay(xbox_encoder encoder);

// Maps standalone mode enum to exact video timing parameters.
// pclk_hz must be the caller's already-in-hand, once-per-main-loop-iteration reading, not a
// fresh adv7511_get_pixel_clock_hz() call -- reading it fresh here would land inside the
// ~500-600ms CTS-disrupted 0-window during an apply. Callers with no meaningful reading on
// hand (HD/576 modes, which never pass interlaced=true) may pass 0 -- see the function body
// for what 0 does.
bool xbox_get_standalone_timing(xbox_encoder encoder, uint8_t standalone_mode, bool widescreen, bool interlaced, uint32_t pclk_hz, XboxVideoTiming *timing);

// Resolve the "declare 16:9" flag for an SD mode: an explicit user_config aspect_ratio
// (16:9 or 4:3) wins; ASPECT_RATIO_AUTO falls back to the caller's signal-derived guess.
// HD callers must not use this -- HD is 16:9 by format regardless of the setting.
bool sd_resolve_widescreen(xbox_encoder encoder, bool signal_widescreen);

// The mode_index of a console mode dword (bits 23:16): SMBus packets and the sniffed dword.
static inline uint8_t xbox_mode_index(uint32_t mode_dword) {
    return (uint8_t)(mode_dword >> 16);
}

// Index (0 to XHD_NUM_TIMING_PROFILES-1, user_config.h) of the timing profile used by the
// most recent successful call to xbox_get_video_timing()/xbox_get_standalone_timing() --
// lets debug.c's "hs"/"vs" CLI calibration commands know which profile_cal_t slot to
// read/write for whatever mode is actually on screen right now. 0xFF if no mode has been
// successfully computed yet this boot.
uint8_t xbox_video_timing_last_profile_index(void);

// True if a timing profile (xbox_video_timing_last_profile_index()) is a 50Hz raster: the
// PAL-I rows, 480- and 576-line alike. Decided by the mode, not the EEPROM region -- a PAL
// console runs PAL-60 modes on the 60Hz NTSC rows.
bool xbox_profile_is_50hz(uint8_t profile);

// Exact refresh (milli-Hz; 0 = unknown) of an applied raster, from the measured pixel clock.
// fifty_hz (xbox_profile_is_50hz()) is exactly 50.000. Otherwise the raster has a standard
// line total (525 / 750 / 1125) and a whole-number line length, so the line length is the
// integer nearest clock / (lines x nominal rate), and the refresh is clock / (line length x
// lines) -- 59.94 vs 60.00 comes out of the real clock.
// Interlaced (1080i) reports the field rate. active_h is the applied raster's (per field).
uint16_t xbox_refresh_mhz(uint16_t active_h, bool interlaced, uint32_t pclk_hz, bool fifty_hz);

// Picture position: the active timing profile's offset from the mode table's base delay
// (horizontal = HS delay in pixels, vertical = VS delay in lines), per encoder. With set, the
// new offset is stored and written live to the ADV7511's DE generator (0x35/0x36, no gate
// cycle); persisted by the config save. *offset returns the stored offset and, if base is
// non-NULL, *base the table's delay (live register minus offset; reads the chip). False with
// no mode applied, or (set) when the resulting delay would leave the register's range.
bool xbox_position(xbox_encoder enc, bool is_hs, int16_t *offset, int16_t *base, bool set);

#endif // __XBOX_VIDEO_TIMING_H__
