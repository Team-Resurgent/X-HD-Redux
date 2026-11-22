// SPDX-License-Identifier: GPL-3.0-only
#include "settings_names.h"
#include "defines.h"

typedef struct {
    const char *full;
    const char *brief;
} cs_name_t;

static const cs_name_t kColorSpaces[XHD_COLOR_SPACE_COUNT] = {
    [XHD_COLOR_SPACE_AUTO]              = { "Auto (EDID / Sink Match)",  "Auto" },
    [XHD_COLOR_SPACE_RGB_FULL]          = { "RGB Full (0-255)",          "RGB Full (0-255)" },
    [XHD_COLOR_SPACE_RGB_LIMITED]       = { "RGB Limited (16-235)",      "RGB Limited (16-235)" },
    [XHD_COLOR_SPACE_YCBCR_PASSTHROUGH] = { "YCbCr 4:4:4 Passthrough",   "YCbCr Passthrough" },
    [XHD_COLOR_SPACE_RGB_PASSTHROUGH]   = { "RGB Direct Passthrough",    "RGB Passthrough" },
    [XHD_COLOR_SPACE_AUTO_SD_FULL]      = { "Auto (SD Forced RGB Full)", "Auto (SD RGB Full)" },
    [XHD_COLOR_SPACE_YCBCR_CSC]         = { "YCbCr 4:4:4 (Adjustable)",  "YCbCr Adjustable" },
};

const char *XhdName_ColorSpace(uint8_t cs)
{
    return cs < XHD_COLOR_SPACE_COUNT ? kColorSpaces[cs].full : "Unknown";
}

const char *XhdName_ColorSpaceShort(uint8_t cs)
{
    return cs < XHD_COLOR_SPACE_COUNT ? kColorSpaces[cs].brief : "Unknown";
}

const char *XhdName_Aspect(uint8_t ar)
{
    switch (ar) {
        case XHD_ASPECT_AUTO: return "Auto (Console / EEPROM Follow)";
        case XHD_ASPECT_16_9: return "16:9 Widescreen";
        case XHD_ASPECT_4_3:  return "4:3 Standard";
        default:              return "Unknown";
    }
}

const char *XhdName_Scan(uint8_t sm)
{
    switch (sm) {
        case XHD_SCAN_AUTO: return "Auto (Underscan if the TV supports it)";
        case XHD_SCAN_TV:   return "TV (Overscan Mode)";
        case XHD_SCAN_PC:   return "PC (Underscan 1:1 Pixel Mapping)";
        default:            return "Unknown";
    }
}

const char *XhdName_Encoder(uint8_t enc)
{
    switch (enc) {
        case 0x8A: return "Conexant CX25871";
        case 0xD4: return "Focus FS454";
        case 0xE0: return "Microsoft Xcalibur";
        default:   return "Unknown / Probing";
    }
}

const char *XhdName_ArbSource(uint8_t src)
{
    switch (src) {
        case 0:  return "Hold Previous";
        case 1:  return "BIOS Mode Packet";
        case 2:  return "BIOS Pending";
        case 3:  return "Path A (Hardware Geometry)";
        case 4:  return "ADV7511 Line Detector";
        case 5:  return "Width Debounce Correction";
        case 6:  return "Blind Fallback Arbiter";
        default: return "Unknown";
    }
}
