#ifndef __XBOX_EEPROM_VIDEO_H__
#define __XBOX_EEPROM_VIDEO_H__

#include <stdint.h>
#include <stdbool.h>

// Pure decoders for the two Xbox factory EEPROM fields the passive EEPROM probe
// (application/smbus_probe.c) captures off the console's own boot-time EEPROM readout. No HAL/I2C/statics
// -- host-testable, mirrors vic_arbiter.c's separation of pure decode logic from the ISR
// code that gathers the raw bytes. Layout verified byte-for-byte against a real bus capture
// and xboxdevwiki.net/EEPROM.

typedef enum {
    XBOX_EEPROM_VIDEO_STD_UNKNOWN = 0,
    XBOX_EEPROM_VIDEO_STD_NTSC_M,
    XBOX_EEPROM_VIDEO_STD_NTSC_J,
    XBOX_EEPROM_VIDEO_STD_PAL_I,
    XBOX_EEPROM_VIDEO_STD_PAL_M,
} xbox_eeprom_video_standard_t;

// raw = the 4-byte little-endian VideoStandard field (EEPROM offset 0x58-0x5B).
xbox_eeprom_video_standard_t xbox_eeprom_decode_video_standard(uint32_t raw);

// True for either PAL variant -- convenience for callers that only care NTSC-vs-PAL, not
// which PAL region.
bool xbox_eeprom_video_standard_is_pal(xbox_eeprom_video_standard_t std);

// --- VideoSettings field (EEPROM offset 0x94-0x97, 4 bytes LE) ---
// Bit map per xboxdevwiki.net/EEPROM (aspect + refresh bits are marked [FIXME] there and
// are frequently just unset -- see xbox_eeprom_resolve_refresh() for how to get the
// effective refresh rate):
//   0x00010000 Widescreen   0x00100000 Letterbox
//   0x00080000 480p enable   0x00020000 720p enable   0x00040000 1080i enable
//   0x00400000 60Hz          0x00800000 50Hz
bool xbox_eeprom_decode_widescreen(uint32_t video_settings);

typedef enum {
    XBOX_EEPROM_REFRESH_UNKNOWN = 0,
    XBOX_EEPROM_REFRESH_50HZ,
    XBOX_EEPROM_REFRESH_60HZ,
} xbox_eeprom_refresh_t;

// Effective dashboard refresh rate. An explicit VideoSettings 50Hz/60Hz bit wins if set;
// otherwise it follows the region: PAL-I modes are 50Hz, all other modes are 60Hz.
// Returns UNKNOWN only when the standard is unrecognised and no
// explicit bit is set. Note: a console configured for PAL-I with explicit 60Hz operation
// keeps the PAL-I standard byte but sets the explicit 60Hz VideoSettings bit -- that path
// resolves to 60Hz here.
xbox_eeprom_refresh_t xbox_eeprom_resolve_refresh(uint32_t video_standard, uint32_t video_settings);

#endif // __XBOX_EEPROM_VIDEO_H__
