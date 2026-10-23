#pragma once

#include "xbox_base.h"
#include <stdbool.h>
#include <stdint.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool has_cea;
    char mfg_id[4];             // 3 letters e.g. "GSM" or "SAM"
    uint16_t product_code;
    uint32_t serial_num;
    uint8_t  week;
    uint16_t year;
    uint8_t  version_major;
    uint8_t  version_minor;
    char monitor_name[16];      // e.g. "LG TV SSCR2"
    
    // Preferred native timing from DTD 1
    bool has_preferred_timing;
    uint16_t pref_width;
    uint16_t pref_height;
    double   pref_refresh;
    uint32_t pref_pclk_khz;
    
    // Physical size in cm
    uint8_t size_h_cm;
    uint8_t size_v_cm;
    
    // CEA Extension (Block 1)
    bool is_hdmi;               // HDMI VSDB 0x000C03 found
    uint8_t cea_revision;
    bool ycbcr444;
    bool ycbcr422;
    bool basic_audio;
    bool underscan;
    uint8_t cec_phys_addr[4];   // e.g. [1, 0, 0, 0]
    
    // SVDs (Short Video Descriptors)
    uint8_t num_vics;
    uint8_t vics[32];
    
    // Common format support flags
    bool supports_480p;
    bool supports_720p;
    bool supports_1080i;
    bool supports_1080p;
    
    // Audio summary
    bool lpcm_2ch;
    bool lpcm_multich;
    bool dolby_digital;
    bool dts;
    uint8_t max_audio_channels;
} edid_parsed_t;

// State management
void EDID_Init(void);
void EDID_RefreshFromBoard(void);
bool EDID_HasData(void);
const edid_parsed_t *EDID_GetParsed(void);

// Controller input handler for EDID tab (Mode toggle, Block scroll)
void EDID_HandleInput(const input_state_t *in);

// Render EDID tab (either Decoded Summary or Raw Hex Dump)
void EDID_Render(void);

#ifdef __cplusplus
}
#endif
