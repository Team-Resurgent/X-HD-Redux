#include "xbox_eeprom_video.h"

#define EEPROM_VS_NTSC_M 0x00400100u
#define EEPROM_VS_NTSC_J 0x00400200u
#define EEPROM_VS_PAL_I  0x00800300u
#define EEPROM_VS_PAL_M  0x00400400u

// The VideoSettings bits decoded here (full map in the header).
#define EEPROM_FLAG_WIDESCREEN 0x00010000u
#define EEPROM_FLAG_60HZ       0x00400000u
#define EEPROM_FLAG_50HZ       0x00800000u

xbox_eeprom_video_standard_t xbox_eeprom_decode_video_standard(uint32_t raw)
{
    switch (raw) {
        case EEPROM_VS_NTSC_M: return XBOX_EEPROM_VIDEO_STD_NTSC_M;
        case EEPROM_VS_NTSC_J: return XBOX_EEPROM_VIDEO_STD_NTSC_J;
        case EEPROM_VS_PAL_I:  return XBOX_EEPROM_VIDEO_STD_PAL_I;
        case EEPROM_VS_PAL_M:  return XBOX_EEPROM_VIDEO_STD_PAL_M;
        default:               return XBOX_EEPROM_VIDEO_STD_UNKNOWN;
    }
}

bool xbox_eeprom_video_standard_is_pal(xbox_eeprom_video_standard_t std)
{
    return std == XBOX_EEPROM_VIDEO_STD_PAL_I || std == XBOX_EEPROM_VIDEO_STD_PAL_M;
}

bool xbox_eeprom_decode_widescreen(uint32_t vs) { return (vs & EEPROM_FLAG_WIDESCREEN) != 0; }
xbox_eeprom_refresh_t xbox_eeprom_resolve_refresh(uint32_t video_standard, uint32_t video_settings)
{
    // An explicit VideoSettings bit wins (covers a PAL-I standard byte paired with an
    // explicit 60Hz bit). Both/neither set -> fall through to the region default.
    bool bit50 = (video_settings & EEPROM_FLAG_50HZ) != 0;
    bool bit60 = (video_settings & EEPROM_FLAG_60HZ) != 0;
    if (bit50 && !bit60) return XBOX_EEPROM_REFRESH_50HZ;
    if (bit60 && !bit50) return XBOX_EEPROM_REFRESH_60HZ;

    switch (xbox_eeprom_decode_video_standard(video_standard)) {
        case XBOX_EEPROM_VIDEO_STD_PAL_I:  return XBOX_EEPROM_REFRESH_50HZ;
        case XBOX_EEPROM_VIDEO_STD_NTSC_M:
        case XBOX_EEPROM_VIDEO_STD_NTSC_J:
        case XBOX_EEPROM_VIDEO_STD_PAL_M:  return XBOX_EEPROM_REFRESH_60HZ;
        default:                           return XBOX_EEPROM_REFRESH_UNKNOWN;
    }
}
