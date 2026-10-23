#include "xbox_eeprom.h"
#include <stdio.h>
#include <string.h>

#define EEPROM_SIZE                 256
#define EEPROM_FACTORY_OFFSET       0x30
#define EEPROM_FACTORY_SIZE         48
#define EEPROM_FACTORY_CHKSUM_OFF   0x30
#define EEPROM_FACTORY_AV_OFF       0x58
#define EEPROM_USER_OFFSET          0x60
#define EEPROM_USER_SIZE            96
#define EEPROM_USER_CHKSUM_OFF      0x60

#define SMBUS_EEPROM_WRITE_ADDR     0xA8
#define SMBUS_EEPROM_READ_ADDR      0xA9

static xbox_eeprom_info_t s_eepromInfo;

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
    // Start with current flags to preserve non-video or PAL bits
    uint32_t flags = s_eepromInfo.current_video_flags;

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

uint32_t EEPROM_ComputeRecommendedAudioFlags(const edid_parsed_t *edid)
{
    uint32_t flags = s_eepromInfo.current_audio_flags;

    // Ensure basic audio channels are at least Surround (or preserve Stereo if set)
    if ((flags & 0xFFFF) == 0) {
        flags |= EEPROM_AUDIO_SURROUND;
    }

    if (edid && edid->valid) {
        // If display or connected AVR supports AC3 (Dolby Digital) or multi-channel audio
        if (edid->dolby_digital || edid->max_audio_channels >= 6) {
            flags |= EEPROM_AUDIO_ENABLE_AC3;
        }
        // If display or AVR supports DTS
        if (edid->dts) {
            flags |= EEPROM_AUDIO_ENABLE_DTS;
        }
    }

    return flags;
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

    // Update AVRegion at offset 0x58 (little-endian DWORD)
    eeprom[EEPROM_FACTORY_AV_OFF]     = (uint8_t)(newStd & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 1] = (uint8_t)((newStd >> 8) & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 2] = (uint8_t)((newStd >> 16) & 0xFF);
    eeprom[EEPROM_FACTORY_AV_OFF + 3] = (uint8_t)((newStd >> 24) & 0xFF);

    // Zero out checksum field (offset 0x30..0x33) prior to recalculation
    memset(&eeprom[EEPROM_FACTORY_CHKSUM_OFF], 0, 4);

    // Recalculate Factory section checksum across 44 payload bytes (0x34 to 0x5F)
    uint32_t newChk = EEPROM_CalculateChecksum(&eeprom[EEPROM_FACTORY_OFFSET + 4], EEPROM_FACTORY_SIZE - 4);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF]     = (uint8_t)(newChk & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 1] = (uint8_t)((newChk >> 8) & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 2] = (uint8_t)((newChk >> 16) & 0xFF);
    eeprom[EEPROM_FACTORY_CHKSUM_OFF + 3] = (uint8_t)((newChk >> 24) & 0xFF);

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

    if (stVid >= 0 && stdSuccess) {
        if (stdChanged) {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "SUCCESS: Switched PAL -> NTSC-M & Enabled HD (480p/720p/1080i/16:9)");
        } else {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "SUCCESS: EEPROM Updated (480p:%s 720p:%s 1080i:%s Wide:%s)",
                     (newVid & EEPROM_VIDEO_HDTV_480P) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_HDTV_720P) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_HDTV_1080I) ? "ON" : "OFF",
                     (newVid & EEPROM_VIDEO_WIDESCREEN) ? "ON" : "OFF");
        }
        return true;
    } else {
        if (!stdSuccess) {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "ERROR: Failed to switch PAL -> NTSC-M region in EEPROM");
        } else {
            snprintf(s_eepromInfo.status_str, sizeof(s_eepromInfo.status_str),
                     "ERROR: ExSaveNonVolatileSetting failed (NTSTATUS 0x%08X)", (unsigned int)stVid);
        }
        return false;
    }
}

const xbox_eeprom_info_t *EEPROM_GetInfo(void)
{
    return &s_eepromInfo;
}
