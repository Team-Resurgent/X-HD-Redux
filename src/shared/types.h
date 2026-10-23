#ifndef __TYPES_H__
#define __TYPES_H__

#include <stdint.h>

typedef enum xbox_encoder {
    // Deliberately not a real SMBus address (see xboxdevwiki.net/SMBus for the 3 real
    // ones below) -- a genuine "not yet determined" placeholder for a truly first-ever
    // boot with nothing persisted yet, so no single real encoder gets treated as a more
    // plausible default than the others. application/main.c starts from it when page 63 has none.
    ENCODER_UNKNOWN  = 0x00,
    ENCODER_CONEXANT = 0x8A,
    ENCODER_FOCUS    = 0xD4,
    ENCODER_XCALIBUR = 0xE0
} xbox_encoder;

typedef enum {
    COLOR_SPACE_AUTO = 0,
    COLOR_SPACE_RGB_FULL = 1,
    COLOR_SPACE_RGB_LIMITED = 2,
    COLOR_SPACE_PASSTHROUGH = 3,
    // The GPU/encoder is putting RGB (not YCbCr) on the digital bus directly.
    // Pass it straight through with NO CSC, but tell the sink RGB
    // (0x16[0]=0, AVI DB1 [6:5]=00). PASSTHROUGH above also bypasses the CSC but labels
    // the stream YCbCr, so on an RGB source it shows wrong colours.
    COLOR_SPACE_RGB_PASSTHROUGH = 4,
    // AUTO, except every SD mode is forced to RGB Full (CscYuvToRgb601, 16-235 -> 0-255,
    // Q=Full) on HDMI and DVI alike, for sinks that ignore Q and show 16 as gray. HD and
    // RGB-input detection behave exactly as AUTO.
    COLOR_SPACE_AUTO_SD_RGB_FULL = 5,
    // Highest valid value -- every range check (user_config load, SMBus write, CLI) uses
    // this, so a new mode can't be silently dropped by a stale bound again (the SMBus
    // check once lagged RGB_PASSTHROUGH this way).
    COLOR_SPACE_MAX = COLOR_SPACE_AUTO_SD_RGB_FULL
} adv_color_space;

// Defined once in user_config.c (compiled into both images); a static inline here was
// duplicated into every caller.
const char* get_xbox_encoder_name(uint8_t encoder);

#endif // __TYPES_H__
