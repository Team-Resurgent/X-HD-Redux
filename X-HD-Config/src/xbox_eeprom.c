// SPDX-License-Identifier: GPL-3.0-only
#include "xbox_eeprom.h"
#include <stdio.h>
#include <string.h>

#define SMBUS_EEPROM_WRITE_ADDR     0xA8
#define SMBUS_EEPROM_READ_ADDR      0xA9

static xbox_eeprom_info_t s_eepromInfo;

void EEPROM_Init(void)
{
    memset(&s_eepromInfo, 0, sizeof(s_eepromInfo));
    s_eepromInfo.valid = false;
    EEPROM_Refresh(NULL);
}

void EEPROM_Refresh(const edid_parsed_t *edid)
{
    ULONG type = 0;
    ULONG resLen = 0;
    uint32_t vid = 0;
    uint32_t aud = 0;
    uint32_t std = 0;

    NTSTATUS stVid = ExQueryNonVolatileSetting(XC_VIDEO_FLAGS, &type, &vid, sizeof(vid), &resLen);
    NTSTATUS stAud = ExQueryNonVolatileSetting(XC_AUDIO_FLAGS, &type, &aud, sizeof(aud), &resLen);
    NTSTATUS stStd = ExQueryNonVolatileSetting(XC_FACTORY_AV_REGION, &type, &std, sizeof(std), &resLen);

    if (stVid >= 0) {
        s_eepromInfo.valid = true;
        s_eepromInfo.current_video_flags = vid;
    } else {
        s_eepromInfo.valid = false;
        s_eepromInfo.current_video_flags = 0;
    }

    if (stAud >= 0) {
        s_eepromInfo.current_audio_flags = aud;
    } else {
        s_eepromInfo.current_audio_flags = 0;
    }

    if (stStd >= 0 && std != 0) {
        s_eepromInfo.video_standard = std;
    } else {
        // Fallback: Read directly from SMBus EEPROM at offset 0x58
        uint32_t smbStd = 0;
        bool smbOk = true;
        for (int b = 0; b < 4; b++) {
            DWORD val = 0;
            if (HalReadSMBusValue(SMBUS_EEPROM_READ_ADDR, (UCHAR)(EEPROM_FACTORY_AV_OFF + b), FALSE, &val) >= 0) {
                smbStd |= ((uint32_t)(val & 0xFF)) << (b * 8);
            } else {
                smbOk = false;
                break;
            }
        }
        if (smbOk && smbStd != 0 && smbStd != 0xFFFFFFFF) {
            s_eepromInfo.video_standard = smbStd;
        } else {
            s_eepromInfo.video_standard = EEPROM_VIDEO_STD_NTSC_M;
        }
    }

    s_eepromInfo.pal_detected = EEPROM_IsPAL(s_eepromInfo.video_standard);
    s_eepromInfo.recommended_video_standard = EEPROM_ComputeRecommendedVideoStandard(s_eepromInfo.video_standard);
    s_eepromInfo.video_standard_switch_needed = (s_eepromInfo.video_standard != s_eepromInfo.recommended_video_standard);

    s_eepromInfo.recommended_video_flags = EEPROM_ComputeRecommendedVideoFlags(edid);
    s_eepromInfo.recommended_audio_flags = EEPROM_ComputeRecommendedAudioFlags(edid);
}

uint32_t EEPROM_ComputeRecommendedVideoFlags(const edid_parsed_t *edid)
{
    return EEPROM_RecommendVideoFlags(s_eepromInfo.current_video_flags, edid);
}

uint32_t EEPROM_ComputeRecommendedAudioFlags(const edid_parsed_t *edid)
{
    eeprom_sink_audio_t sink = { 0 };
    if (edid) {
        sink.valid = edid->valid;
        sink.is_hdmi = edid->is_hdmi;
        sink.basic_audio = edid->basic_audio;
        sink.dolby_digital = edid->dolby_digital;
        sink.dts = edid->dts;
        sink.max_channels = edid->max_audio_channels;
    }
    return EepromAudio_Recommend(s_eepromInfo.current_audio_flags, &sink);
}

bool EEPROM_WriteVideoStandard(uint32_t newStd)
{
    // Layer 1: Attempt standard kernel ExSaveNonVolatileSetting(XC_FACTORY_AV_REGION)
    NTSTATUS st = ExSaveNonVolatileSetting(XC_FACTORY_AV_REGION, REG_DWORD, &newStd, sizeof(newStd));
    if (st >= 0) {
        ULONG type = 0, resLen = 0;
        uint32_t check = 0;
        if (ExQueryNonVolatileSetting(XC_FACTORY_AV_REGION, &type, &check, sizeof(check), &resLen) >= 0 && check == newStd) {
            return true;
        }
    }

    // Layer 2: Read full EEPROM (256 bytes), patch Factory section & checksum, write via XC_MAX_ALL
    uint8_t eeprom[EEPROM_SIZE];
    memset(eeprom, 0, sizeof(eeprom));

    ULONG type = 0, resLen = 0;
    NTSTATUS stRead = ExQueryNonVolatileSetting(XC_MAX_ALL, &type, eeprom, EEPROM_SIZE, &resLen);
    if (stRead < 0 || resLen != EEPROM_SIZE) {
        // Fallback: Read entire EEPROM over SMBus address 0xA9
        for (int i = 0; i < EEPROM_SIZE; i++) {
            DWORD b = 0;
            if (HalReadSMBusValue(SMBUS_EEPROM_READ_ADDR, (UCHAR)i, FALSE, &b) < 0) {
                return false;
            }
            eeprom[i] = (uint8_t)(b & 0xFF);
        }
    }

    uint8_t original[EEPROM_SIZE];
    memcpy(original, eeprom, sizeof(original));

    EEPROM_SetFactoryRegion(eeprom, newStd);

    NTSTATUS stWrite = ExSaveNonVolatileSetting(XC_MAX_ALL, REG_BINARY, eeprom, EEPROM_SIZE);
    if (stWrite >= 0) {
        ULONG type2 = 0, resLen2 = 0;
        uint32_t check2 = 0;
        if (ExQueryNonVolatileSetting(XC_FACTORY_AV_REGION, &type2, &check2, sizeof(check2), &resLen2) >= 0 && check2 == newStd) {
            return true;
        }
    }

    // Layer 3: Direct SMBus hardware write to 24WC02 EEPROM at address 0xA8. Only the
    // bytes that change (region and checksum, at most 8) are written, so a failure part-way
    // through leaves as little of the factory section touched as possible.
    bool smbWriteOk = true;
    for (int off = EEPROM_FACTORY_OFFSET; off < (EEPROM_FACTORY_OFFSET + EEPROM_FACTORY_SIZE); off++) {
        if (eeprom[off] == original[off]) {
            continue;
        }
        NTSTATUS stHal = HalWriteSMBusValue(SMBUS_EEPROM_WRITE_ADDR, (UCHAR)off, FALSE, (DWORD)eeprom[off]);
        if (stHal < 0) {
            smbWriteOk = false;
            break;
        }
        Sleep(5); // 24WC02 EEPROM write cycle time (max 5ms)
    }

    if (smbWriteOk) {
        uint32_t readBack = 0;
        for (int b = 0; b < 4; b++) {
            DWORD val = 0;
            if (HalReadSMBusValue(SMBUS_EEPROM_READ_ADDR, (UCHAR)(EEPROM_FACTORY_AV_OFF + b), FALSE, &val) >= 0) {
                readBack |= ((uint32_t)(val & 0xFF)) << (b * 8);
            }
        }
        if (readBack == newStd) {
            return true;
        }
    }

    return false;
}

bool EEPROM_ApplyRecommended(const edid_parsed_t *edid)
{
    uint32_t newVid = EEPROM_ComputeRecommendedVideoFlags(edid);
    uint32_t newAud = EEPROM_ComputeRecommendedAudioFlags(edid);
    bool stdChanged = false;
    bool stdSuccess = true;

    if (s_eepromInfo.video_standard_switch_needed) {
        stdChanged = true;
        stdSuccess = EEPROM_WriteVideoStandard(s_eepromInfo.recommended_video_standard);
    }

    NTSTATUS stVid = ExSaveNonVolatileSetting(XC_VIDEO_FLAGS, REG_DWORD, &newVid, sizeof(newVid));
    NTSTATUS stAud = ExSaveNonVolatileSetting(XC_AUDIO_FLAGS, REG_DWORD, &newAud, sizeof(newAud));

    EEPROM_Refresh(edid);

    if (stVid >= 0 && stAud >= 0 && stdSuccess) {
        char audio[40];
        snprintf(audio, sizeof(audio), "%s%s%s", EepromAudio_ModeName(newAud),
                 (newAud & EEPROM_AUDIO_ENABLE_AC3) ? " AC3" : "",
                 (newAud & EEPROM_AUDIO_ENABLE_DTS) ? " DTS" : "");
        if (stdChanged) {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "SUCCESS: Switched PAL -> NTSC-M & Enabled HD (480p/720p/1080i/16:9) | Audio: %s", audio);
        } else {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "SUCCESS: EEPROM Updated (480p:%s 720p:%s 1080i:%s Wide:%s | Audio: %s)",
                     (newVid & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF", audio);
        }
        return true;
    } else {
        if (!stdSuccess) {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "ERROR: Failed to switch PAL -> NTSC-M region in EEPROM");
        } else {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "ERROR: ExSaveNonVolatileSetting failed (video 0x%08X, audio 0x%08X)",
                     (unsigned int)stVid, (unsigned int)stAud);
        }
        return false;
    }
}

const xbox_eeprom_info_t *EEPROM_GetInfo(void)
{
    return &s_eepromInfo;
}
