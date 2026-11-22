// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include "ui_edid.h"
#include "eeprom_audio.h"
#include "eeprom_video.h"
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
uint32_t EEPROM_ComputeRecommendedVideoFlags(const edid_parsed_t *edid);
uint32_t EEPROM_ComputeRecommendedAudioFlags(const edid_parsed_t *edid);
bool EEPROM_WriteVideoStandard(uint32_t newStd);
bool EEPROM_ApplyRecommended(const edid_parsed_t *edid);
const xbox_eeprom_info_t *EEPROM_GetInfo(void);

#ifdef __cplusplus
}
#endif
