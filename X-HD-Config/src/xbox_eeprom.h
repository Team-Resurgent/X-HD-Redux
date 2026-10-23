#pragma once

#include "xbox_base.h"
#include "ui_edid.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Xbox EEPROM Non-Volatile Setting Indices (xconfig.h / xboxp.h)
#ifndef XC_VIDEO_FLAGS
#define XC_VIDEO_FLAGS 8
#endif

#ifndef XC_AUDIO_FLAGS
#define XC_AUDIO_FLAGS 9
#endif

#ifndef XC_FACTORY_AV_REGION
#define XC_FACTORY_AV_REGION 259
#endif

#ifndef XC_MAX_ALL
#define XC_MAX_ALL 0xFFFF
#endif

#ifndef REG_BINARY
#define REG_BINARY 3
#endif

#ifndef REG_DWORD
#define REG_DWORD 4
#endif

// Canonical Xbox Motherboard EEPROM Video Flags (Offset 0x94)
// Verified against Ryzee119/xbox_eeprom_online and xboxdevwiki.net/EEPROM
#define EEPROM_VIDEO_WIDESCREEN     0x00010000  // Bit 16: 16:9 Widescreen aspect
#define EEPROM_VIDEO_HDTV_720P      0x00020000  // Bit 17: 720p HDTV enabled
#define EEPROM_VIDEO_HDTV_1080I     0x00040000  // Bit 18: 1080i HDTV enabled
#define EEPROM_VIDEO_HDTV_480P      0x00080000  // Bit 19: 480p HDTV enabled
#define EEPROM_VIDEO_LETTERBOX      0x00100000  // Bit 20: 4:3 Letterbox
#define EEPROM_VIDEO_PAL_60HZ       0x00400000  // Bit 22: PAL 60Hz enabled
#define EEPROM_VIDEO_PAL_50HZ       0x00800000  // Bit 23: PAL 50Hz enabled

// Canonical Xbox Motherboard EEPROM Audio Flags (Offset 0x98)
#define EEPROM_AUDIO_STEREO         0x00000000  // Bits 0-1: Stereo
#define EEPROM_AUDIO_MONO           0x00000001  // Bits 0-1: Mono
#define EEPROM_AUDIO_SURROUND       0x00000002  // Bits 0-1: Dolby Surround
#define EEPROM_AUDIO_ENABLE_AC3     0x00010000  // Bit 16: Dolby Digital (AC3)
#define EEPROM_AUDIO_ENABLE_DTS     0x00020000  // Bit 17: DTS

// Factory Video Standard / Region (Offset 0x58)
#define EEPROM_VIDEO_STD_NTSC_M     0x00400100  // North America
#define EEPROM_VIDEO_STD_NTSC_J     0x00400200  // Japan
#define EEPROM_VIDEO_STD_PAL_I      0x00800300  // Europe / Australia
#define EEPROM_VIDEO_STD_PAL_M      0x00400400  // Brazil

typedef struct {
    bool valid;
    uint32_t video_standard;             // Current factory AVRegion in EEPROM (0x58)
    uint32_t recommended_video_standard; // Target AVRegion (NTSC-M if PAL detected, else current)
    bool pal_detected;                   // True if current video_standard is PAL
    bool video_standard_switch_needed;   // True if video_standard != recommended_video_standard
    uint32_t current_video_flags;
    uint32_t recommended_video_flags;
    uint32_t current_audio_flags;
    uint32_t recommended_audio_flags;
    char status_str[256];
} xbox_eeprom_info_t;

void EEPROM_Init(void);
void EEPROM_Refresh(const edid_parsed_t *edid);
bool EEPROM_IsPAL(uint32_t std);
uint32_t EEPROM_ComputeRecommendedVideoStandard(uint32_t current_std);
uint32_t EEPROM_CalculateChecksum(const uint8_t *data, size_t length);
uint32_t EEPROM_ComputeRecommendedVideoFlags(const edid_parsed_t *edid);
uint32_t EEPROM_ComputeRecommendedAudioFlags(const edid_parsed_t *edid);
bool EEPROM_WriteVideoStandard(uint32_t newStd);
bool EEPROM_ApplyRecommended(const edid_parsed_t *edid);
const xbox_eeprom_info_t *EEPROM_GetInfo(void);
const char *EEPROM_GetVideoStandardStr(uint32_t std);

#ifdef __cplusplus
}
#endif
