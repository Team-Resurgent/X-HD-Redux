// SPDX-License-Identifier: GPL-3.0-only
#include "eeprom_audio.h"

bool EepromAudio_SinkHasAudio(const eeprom_sink_audio_t *sink)
{
    return sink && sink->valid && sink->is_hdmi && (sink->basic_audio || sink->max_channels > 0);
}

uint32_t EepromAudio_Recommend(uint32_t current, const eeprom_sink_audio_t *sink)
{
    if (!EepromAudio_SinkHasAudio(sink)) {
        return current;
    }
    uint32_t flags = current & ~(EEPROM_AUDIO_MODE_MASK | EEPROM_AUDIO_ENABLE_AC3 | EEPROM_AUDIO_ENABLE_DTS);
    flags |= (sink->max_channels >= 6) ? EEPROM_AUDIO_SURROUND : EEPROM_AUDIO_STEREO;
    if (sink->dolby_digital) {
        flags |= EEPROM_AUDIO_ENABLE_AC3;
    }
    if (sink->dts) {
        flags |= EEPROM_AUDIO_ENABLE_DTS;
    }
    return flags;
}

const char *EepromAudio_ModeName(uint32_t flags)
{
    switch (flags & EEPROM_AUDIO_MODE_MASK) {
        case EEPROM_AUDIO_STEREO:   return "Stereo";
        case EEPROM_AUDIO_MONO:     return "Mono";
        case EEPROM_AUDIO_SURROUND: return "Surround";
        default:                    return "Other";
    }
}
