// SPDX-License-Identifier: GPL-3.0-only
// The console EEPROM's audio flags (XC_AUDIO_FLAGS, offset 0x98) and the setting recommended
// for the HDMI sink. No kernel calls, so the host tests build it as is.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EEPROM_AUDIO_MODE_MASK      0x0000FFFF  // Low word: channel mode
#define EEPROM_AUDIO_STEREO         0x00000000
#define EEPROM_AUDIO_MONO           0x00000001
#define EEPROM_AUDIO_SURROUND       0x00000002  // Dolby Surround (matrixed into the stereo pair)
#define EEPROM_AUDIO_ENABLE_AC3     0x00010000  // Bit 16: Dolby Digital bitstream
#define EEPROM_AUDIO_ENABLE_DTS     0x00020000  // Bit 17: DTS bitstream

// What the sink's EDID says about audio.
typedef struct {
    bool valid;            // an EDID was read
    bool is_hdmi;          // HDMI VSDB present (DVI carries no audio)
    bool basic_audio;      // CEA byte 3 bit 6: 2-channel LPCM
    bool dolby_digital;    // SAD format 2
    bool dts;              // SAD format 7
    uint8_t max_channels;  // highest channel count over all SADs, 0 if none
} eeprom_sink_audio_t;

// True when the sink takes audio at all; without it the recommendation keeps the current flags.
bool EepromAudio_SinkHasAudio(const eeprom_sink_audio_t *sink);

// Channel mode from the channel count (6 or more -> Surround, else Stereo); AC3 and DTS exactly
// as the EDID declares them; bits outside these are kept.
uint32_t EepromAudio_Recommend(uint32_t current, const eeprom_sink_audio_t *sink);

// "Stereo", "Mono", "Surround" or "Other" for an unknown low word.
const char *EepromAudio_ModeName(uint32_t flags);

#ifdef __cplusplus
}
#endif
