// SPDX-License-Identifier: GPL-3.0-only
// The console EEPROM's video side: the factory AV region (0x58, with the factory-section
// checksum at 0x30) and the user video flags (XC_VIDEO_FLAGS), and what to recommend for the
// HDMI sink. No kernel calls, so the host tests build it as is; xbox_eeprom.c does the reading and writing.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edid_parse.h"

#ifdef __cplusplus
extern "C" {
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

// Factory Video Standard / Region (Offset 0x58)
#define EEPROM_VIDEO_STD_NTSC_M     0x00400100  // North America
#define EEPROM_VIDEO_STD_NTSC_J     0x00400200  // Japan
#define EEPROM_VIDEO_STD_PAL_I      0x00800300  // Europe / Australia
#define EEPROM_VIDEO_STD_PAL_M      0x00400400  // Brazil

#define EEPROM_SIZE                 256
#define EEPROM_FACTORY_OFFSET       0x30    // factory section: checksum, then 44 bytes
#define EEPROM_FACTORY_SIZE         48
#define EEPROM_FACTORY_AV_OFF       0x58    // AV region (video standard), little-endian

// PAL-I, PAL-M, or any region whose standard index (bits 8-15) is 3/4 or with the 50 Hz bit.
bool EEPROM_IsPAL(uint32_t std);
// NTSC-M for a PAL console (the dashboard hides HD modes on PAL); otherwise the current one
// (NTSC-M when unset).
uint32_t EEPROM_ComputeRecommendedVideoStandard(uint32_t current_std);
const char *EEPROM_GetVideoStandardStr(uint32_t std);
// The Xbox section checksum: ~(32-bit little-endian word sum, carries folded back in).
uint32_t EEPROM_CalculateChecksum(const uint8_t *data, size_t length);
// 480p, 720p, PAL-60 on, letterbox and PAL-50 off; 1080i when the sink lists it, has 1080
// lines, or there is no EDID; widescreen unless the sink's native timing is 4:3 or narrower.
// Other bits of `current` are kept. `edid` may be NULL.
uint32_t EEPROM_RecommendVideoFlags(uint32_t current, const edid_parsed_t *edid);
// Writes `std` as the AV region of a 256-byte EEPROM image and recomputes the factory
// section's checksum.
void EEPROM_SetFactoryRegion(uint8_t eeprom[EEPROM_SIZE], uint32_t std);

#ifdef __cplusplus
}
#endif
