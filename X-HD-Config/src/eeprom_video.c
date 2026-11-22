// SPDX-License-Identifier: GPL-3.0-only
#include "eeprom_video.h"
#include <string.h>

#define EEPROM_FACTORY_CHKSUM_OFF   0x30

bool EEPROM_IsPAL(uint32_t std)
{
    if (std == EEPROM_VIDEO_STD_PAL_I || std == EEPROM_VIDEO_STD_PAL_M) {
        return true;
    }
    // AV standard byte representation check (bits 8-15 contain standard index: 1=NTSC-M, 2=NTSC-J, 3=PAL-I, 4=PAL-M)
    uint32_t stdIndex = (std >> 8) & 0xFF;
    if (stdIndex == 3 || stdIndex == 4) {
        return true;
    }
    // High word bit 23 check: 0x00800000 indicates 50Hz / PAL family
    if ((std & 0x00800000) != 0) {
        return true;
    }
    return false;
}

uint32_t EEPROM_ComputeRecommendedVideoStandard(uint32_t current_std)
{
    if (EEPROM_IsPAL(current_std)) {
        return EEPROM_VIDEO_STD_NTSC_M;
    }
    // If already NTSC (NTSC-M 0x00400100 or NTSC-J 0x00400200), keep current standard
    return current_std ? current_std : EEPROM_VIDEO_STD_NTSC_M;
}

const char *EEPROM_GetVideoStandardStr(uint32_t std)
{
    switch (std) {
        case EEPROM_VIDEO_STD_NTSC_M: return "NTSC-M (North America)";
        case EEPROM_VIDEO_STD_NTSC_J: return "NTSC-J (Japan)";
        case EEPROM_VIDEO_STD_PAL_I:  return "PAL-I (Europe/Australia)";
        case EEPROM_VIDEO_STD_PAL_M:  return "PAL-M (Brazil)";
        default:
            if (EEPROM_IsPAL(std)) {
                return "PAL (Custom/Other)";
            }
            return "NTSC (Standard)";
    }
}

uint32_t EEPROM_CalculateChecksum(const uint8_t *data, size_t length)
{
    if (!data || length < 4) {
        return 0;
    }
    uint64_t sum = 0;
    size_t count = length / 4;
    for (size_t i = 0; i < count; i++) {
        uint32_t val = (uint32_t)data[i * 4] |
                       ((uint32_t)data[i * 4 + 1] << 8) |
                       ((uint32_t)data[i * 4 + 2] << 16) |
                       ((uint32_t)data[i * 4 + 3] << 24);
        sum += val;
    }
    uint32_t high = (uint32_t)(sum >> 32);
    uint32_t low = (uint32_t)(sum & 0xFFFFFFFFULL);
    uint64_t folded = (uint64_t)high + (uint64_t)low;
    while (folded > 0xFFFFFFFFULL) {
        folded = (folded & 0xFFFFFFFFULL) + (folded >> 32);
    }
    return ~(uint32_t)folded;
}

uint32_t EEPROM_RecommendVideoFlags(uint32_t current, const edid_parsed_t *edid)
{
    // Start with current flags to preserve non-video or PAL bits
    uint32_t flags = current;

    // For an HDMI-modded Xbox, enable 480p, 720p, and 16:9 widescreen in upper word (bits 16-23)
    flags |= EEPROM_VIDEO_HDTV_480P;
    flags |= EEPROM_VIDEO_HDTV_720P;
    flags |= EEPROM_VIDEO_WIDESCREEN;
    flags &= ~EEPROM_VIDEO_LETTERBOX; // Disable letterbox

    if (edid && edid->valid) {
        // 1080i: Enable if sink EDID explicitly declares 1080i support or >= 1080 vertical lines
        if (edid->supports_1080i || edid->pref_height >= 1080) {
            flags |= EEPROM_VIDEO_HDTV_1080I;
        }

        // If native display is strictly 4:3 (e.g. 1024x768 / 640x480 CRT / projector)
        if (edid->pref_width > 0 && edid->pref_height > 0) {
            if ((uint32_t)edid->pref_width * 3 <= (uint32_t)edid->pref_height * 4) {
                flags &= ~EEPROM_VIDEO_WIDESCREEN;
            } else {
                flags |= EEPROM_VIDEO_WIDESCREEN;
            }
        }
    } else {
        // Default to enabling 1080i as well when no EDID is attached
        flags |= EEPROM_VIDEO_HDTV_1080I;
    }

    // Ensure PAL 60Hz flag is set so PAL games render in 60Hz smooth raster
    flags |= EEPROM_VIDEO_PAL_60HZ;
    // Clear 50Hz interlaced mode flag
    flags &= ~EEPROM_VIDEO_PAL_50HZ;

    return flags;
}

void EEPROM_SetFactoryRegion(uint8_t eeprom[EEPROM_SIZE], uint32_t std)
{
    // Update AVRegion at offset 0x58 (little-endian DWORD)
    eeprom[EEPROM_FACTORY_AV_OFF]     = (uint8_t)(std & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 1] = (uint8_t)((std >> 8) & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 2] = (uint8_t)((std >> 16) & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 3] = (uint8_t)((std >> 24) & 0xFF);

    // Zero out checksum field (offset 0x30..0x33) prior to recalculation
    memset(&eeprom[EEPROM_FACTORY_CHKSUM_OFF], 0, 4);

    // Recalculate Factory section checksum across 44 payload bytes (0x34 to 0x5F)
    uint32_t newChk = EEPROM_CalculateChecksum(&eeprom[EEPROM_FACTORY_OFFSET + 4], EEPROM_FACTORY_SIZE - 4);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF]     = (uint8_t)(newChk & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 1] = (uint8_t)((newChk >> 8) & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 2] = (uint8_t)((newChk >> 16) & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 3] = (uint8_t)((newChk >> 24) & 0xFF);
}
