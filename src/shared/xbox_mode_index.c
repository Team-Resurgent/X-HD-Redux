#include "xbox_mode_index.h"
#include "types.h"

static const uint8_t s_mode[0x13] = {
    [0x00] = FAM_NONE,
    [0x01] = FAM_SD_640     | MODE_640,                                 // NTSC 640x480
    [0x02] = FAM_SD_720x480,                                            // NTSC 720x480
    [0x03] = FAM_SD_640     | MODE_640 | MODE_PAL50 | MODE_BIOS_AUTH,   // PAL 640x480 @50
    [0x04] = FAM_SD_720x480 | MODE_PAL50 | MODE_BIOS_AUTH,              // PAL 720x480 @50
    [0x05] = FAM_SD_720x576 | MODE_640 | MODE_PAL50,                    // PAL 640x576
    [0x06] = FAM_SD_720x576 | MODE_PAL50,                               // PAL 720x576
    [0x07] = FAM_SD_720x480 | MODE_480P,                                // 480p (640 on Xcalibur)
    [0x08] = FAM_SD_720x480 | MODE_480P,                                // 480p 720
    [0x09] = FAM_HD_720p,                                               // 960x720
    [0x0A] = FAM_HD_720p,
    [0x0B] = FAM_HD_720p,
    [0x0C] = FAM_HD_1080i,                                              // 1440x1080i
    [0x0D] = FAM_HD_1080i,
    [0x0E] = FAM_HD_1080i,
    [0x0F] = FAM_SD_640     | MODE_640 | MODE_BIOS_AUTH,                // NTSC 640 "1.1:1 PAR"
    [0x10] = FAM_SD_640     | MODE_640 | MODE_PAL50 | MODE_BIOS_AUTH,   // PAL 640 "1.1:1 PAR"
    [0x11] = FAM_SD_720x480 | MODE_480P,                                // 480p 720
    [0x12] = FAM_SD_720x576 | MODE_640 | MODE_PAL50,                    // PAL 640x576 PAR
};

uint8_t xbox_mode_flags(uint8_t idx, uint8_t encoder) {
    if (idx >= sizeof s_mode) return FAM_NONE;
    if (idx == 0x07 && encoder == (uint8_t)ENCODER_XCALIBUR) return FAM_SD_640 | MODE_640 | MODE_480P;
    return s_mode[idx];
}
