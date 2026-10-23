#pragma once

#ifndef __XGMATH_INL__
#define __XGMATH_INL__
#endif
#ifndef __D3DX8MATH_INL__
#define __D3DX8MATH_INL__
#endif

#include <stdint.h>
#include <stdbool.h>

// X-HD SMBus 7-bit slave address: 0x69 (8-bit write 0xD2, read 0xD3)
#define XHD_SMBUS_ADDR                  0xD2

// --- Read Opcodes (0 - 127) ---
#define XHD_OP_READ_CONFIG              0
#define XHD_OP_READ_VERSION1            1
#define XHD_OP_READ_VERSION2            2
#define XHD_OP_READ_VERSION3            3
#define XHD_OP_READ_VERSION4            4
#define XHD_OP_READ_MODE                5
#define XHD_OP_READ_RAM                 6
#define XHD_OP_READ_RAM_PAGE_CRC1       7
#define XHD_OP_READ_RAM_PAGE_CRC2       8
#define XHD_OP_READ_RAM_PAGE_CRC3       9
#define XHD_OP_READ_RAM_PAGE_CRC4       10
#define XHD_OP_READ_COLOR_SPACE         11
#define XHD_OP_READ_EDID                12
#define XHD_OP_READ_PHASE               13
#define XHD_OP_READ_TELEMETRY           15
#define XHD_OP_READ_ASPECT_RATIO        16
#define XHD_OP_READ_SCAN_MODE           17
#define XHD_OP_READ_CEC_STATUS          18
#define XHD_OP_READ_CEC_RX_LEN          19
#define XHD_OP_READ_CEC_RX_DATA         20
#define XHD_OP_READ_SPD_DATA            21
#define XHD_OP_READ_VIDEO_INFO          22
#define XHD_OP_READ_ABOUT               23
#define XHD_OP_READ_LIVE_FLAGS          24
#define XHD_OP_READ_USER_CONFIG         25

// --- Write Opcodes (128 - 255) ---
#define XHD_OP_WRITE_CONFIG             128
#define XHD_OP_WRITE_CONFIG_BANK        129
#define XHD_OP_WRITE_CONFIG_INDEX       130
#define XHD_OP_WRITE_CONFIG_APPLY       131
#define XHD_OP_WRITE_SET_MODE           132
#define XHD_OP_WRITE_READ_PAGE          133
#define XHD_OP_WRITE_RAM                134
#define XHD_OP_WRITE_RAM_BANK           135
#define XHD_OP_WRITE_RAM_INDEX          136
#define XHD_OP_WRITE_RAM_APPLY          137
#define XHD_OP_WRITE_APP_FLASH_MODE     138
#define XHD_OP_WRITE_COLOR_SPACE        139
#define XHD_OP_WRITE_EDID_INDEX         140
#define XHD_OP_WRITE_PHASE              141
#define XHD_OP_WRITE_CONFIG_ACTION      142
#define XHD_OP_WRITE_ASPECT_RATIO       144
#define XHD_OP_WRITE_SCAN_MODE          145
#define XHD_OP_WRITE_CEC_TX_DATA        146
#define XHD_OP_WRITE_CEC_TX_APPLY       147
#define XHD_OP_WRITE_CEC_RX_ACK         148
#define XHD_OP_WRITE_SPD_DATA           149
#define XHD_OP_WRITE_SPD_APPLY          150
#define XHD_OP_WRITE_VIDEO_INFO_LATCH   151
#define XHD_OP_WRITE_WIRE_VIC_MODE      152
#define XHD_OP_WRITE_POSITION_H         153
#define XHD_OP_WRITE_POSITION_V         154
#define XHD_OP_WRITE_ABOUT_INDEX        155
#define XHD_OP_WRITE_CEC_CLOCK_OUT      156
#define XHD_OP_WRITE_CEC_ONE_TOUCH      157
#define XHD_OP_WRITE_AUDIO_MUTE         158
#define XHD_OP_WRITE_FREEZE             159
#define XHD_OP_WRITE_USER_CONFIG_INDEX  160
#define XHD_OP_WRITE_TELEMETRY_INDEX    161
#define XHD_OP_WRITE_CEC_STANDBY        162

// --- Action codes for WRITE_CONFIG_ACTION (142) ---
#define XHD_ACTION_SAVE_FLASH           1
#define XHD_ACTION_RESTORE_DEFAULTS     2
#define XHD_ACTION_REAPPLY_MODE         3
#define XHD_ACTION_RELOAD_FLASH         4

// --- Mode codes for READ_MODE (5) / WRITE_SET_MODE (132) ---
#define XHD_MODE_BOOTLOADER             1
#define XHD_MODE_APPLICATION            2

// --- Settings Enums ---
typedef enum {
    XHD_COLOR_SPACE_AUTO                = 0,
    XHD_COLOR_SPACE_RGB_FULL            = 1,
    XHD_COLOR_SPACE_RGB_LIMITED         = 2,
    XHD_COLOR_SPACE_YCBCR_PASSTHROUGH   = 3,
    XHD_COLOR_SPACE_RGB_PASSTHROUGH     = 4,
    XHD_COLOR_SPACE_AUTO_SD_FULL        = 5
} xhd_color_space_e;

typedef enum {
    XHD_ASPECT_AUTO                     = 0,
    XHD_ASPECT_16_9                     = 1,
    XHD_ASPECT_4_3                      = 2
} xhd_aspect_ratio_e;

typedef enum {
    XHD_SCAN_AUTO                       = 0,
    XHD_SCAN_TV                         = 1,
    XHD_SCAN_PC                         = 2
} xhd_scan_mode_e;

typedef enum {
    XHD_WIRE_VIC_HONEST                 = 0,
    XHD_WIRE_VIC_AUTO                   = 1
} xhd_wire_vic_mode_e;

typedef enum {
    XHD_ENCODER_XCALIBUR                = 0,
    XHD_ENCODER_CONEXANT                = 1,
    XHD_ENCODER_FOCUS                   = 2,
    XHD_ENCODER_UNKNOWN                 = 0xFF
} xhd_encoder_e;

// Video Info Flags (byte offset 3)
#define XHD_VI_FLAG_INTERLACED          0x01
#define XHD_VI_FLAG_16_9                0x02
#define XHD_VI_FLAG_ANAMORPHIC          0x04
#define XHD_VI_FLAG_FROM_BIOS           0x08
#define XHD_VI_FLAG_HDMI_SINK           0x10
#define XHD_VI_FLAG_APPLIED             0x20
#define XHD_VI_FLAG_PCLK_LOCKED         0x40
#define XHD_VI_FLAG_WIRE_VIC_AUTO       0x80

// Telemetry HW Flags (byte offset 8)
#define XHD_TLM_FLAG_HPD                0x01
#define XHD_TLM_FLAG_MONITOR_SENSE      0x02
#define XHD_TLM_FLAG_VIDEO_UNGATED      0x08
#define XHD_TLM_FLAG_AUDIO_ON           0x10
#define XHD_TLM_FLAG_FREEZE             0x20
#define XHD_TLM_FLAG_PLL_LOCKED         0x80

// CEC Status Flags (READ_CEC_STATUS 18)
#define XHD_CEC_STATUS_TX_BUSY          0x01
#define XHD_CEC_STATUS_LAST_TX_ACKED    0x02
#define XHD_CEC_STATUS_TX_LOST_ARB      0x04
#define XHD_CEC_STATUS_TX_TIMEOUT       0x08
#define XHD_CEC_STATUS_RX_PENDING       0x10
#define XHD_CEC_STATUS_CLOCK_OUT_ON     0x20
#define XHD_CEC_STATUS_ONE_TOUCH_ON     0x40
#define XHD_CEC_STATUS_STANDBY_ON       0x80

// Live Flags (READ_LIVE_FLAGS 24)
#define XHD_LIVE_FLAG_AUDIO_MUTE_HELD   0x01
#define XHD_LIVE_FLAG_FREEZE_ON         0x02

// Number of video timing profile calibration slots in Page 63
#define XHD_NUM_TIMING_PROFILES         18
#define XHD_SPD_NAME_LEN                24

#pragma pack(push, 1)

// --- Latched Video Info Struct (24 Bytes, READ_VIDEO_INFO = 22) ---
typedef struct {
    uint8_t   struct_version;     // 0  : 1
    uint8_t   struct_size;        // 1  : 24
    uint8_t   change_count;       // 2  : mode change counter
    uint8_t   flags;              // 3  : XHD_VI_FLAG_*
    uint16_t  width;              // 4  : applied raster width
    uint16_t  height;             // 6  : applied raster height (1080 for 1080i)
    uint32_t  pixel_clock_hz;     // 8  : measured input clock in Hz
    uint16_t  refresh_mhz;        // 12 : refresh in milli-Hz (59940 = 59.94 Hz)
    uint8_t   wire_vic;           // 14 : VIC sent to HDMI sink
    uint8_t   internal_vic;       // 15 : internal CEA family VIC
    uint8_t   detected_vic;       // 16 : ADV7511 line detector VIC
    uint8_t   console_mode_index; // 17 : BIOS mode index (0xFF = standalone)
    uint8_t   color_space;        // 18 : 1=Full, 2=Limited, 3=YCbCr, 4=RGB pass
    int8_t    pos_h;              // 19 : horizontal picture position offset
    int8_t    pos_v;              // 20 : vertical picture position offset
    uint8_t   reserved[3];        // 21
} XHD_VIDEO_INFO;

// --- Telemetry Diagnostics Struct (20 Bytes, READ_TELEMETRY = 15) ---
typedef struct {
    uint32_t  pixel_clock_hz;     // 0  : measured input clock
    uint8_t   detected_input_vic; // 4  : ADV7511 line detector (0x3E)
    uint8_t   arb_source;         // 5  : 0=hold, 1=BIOS, 2=pending, 3=Path A, 4=detector, 5=width, 6=blind
    uint8_t   arb_target_vic;     // 6  : target internal VIC
    uint8_t   probed_encoder;     // 7  : 0x8A Conexant, 0xD4 Focus, 0xE0 Xcalibur
    uint8_t   hw_flags;           // 8  : XHD_TLM_FLAG_*
    uint8_t   fault_code;         // 9  : error fault code (0 = none)
    uint16_t  uptime_s;           // 10 : board uptime in seconds
    uint16_t  mode_change_count;  // 12 : applied mode changes since boot
    uint8_t   pll_loss_count;     // 14 : PLL loss events
    uint8_t   gate_cycle_count;   // 15 : TMDS input gate cycles
    uint8_t   blind_event_count;  // 16 : detector blind events
    uint8_t   struct_version;     // 17 : 1
    uint8_t   struct_size;        // 18 : 20
    uint8_t   reserved;           // 19
} XHD_TELEMETRY;

// --- About Hardware & Firmware Info Block (60 Bytes, READ_ABOUT = 23) ---
typedef struct {
    uint8_t   struct_version;     // 0  : 1
    uint8_t   struct_size;        // 1  : 60
    uint8_t   chip_id;            // 2  : ADV7511 reg 0x00
    uint8_t   chip_rev;           // 3  : ADV7511 reg 0xF5
    uint8_t   mcu_uid[12];        // 4  : STM32 96-bit unique ID
    char      git[24];            // 16 : Git commit hash string
    char      built[20];          // 40 : Build date/time YYYY-MM-DDTHH:MM:SS
} XHD_ABOUT;

// --- Page 63 User Configuration Layout (238 Bytes, READ_USER_CONFIG = 25) ---
typedef struct {
    int16_t   hs_delay_offset;    // HS delay delta in pixels
    int16_t   vs_delay_offset;    // VS delay delta in lines
} profile_cal_t;

typedef struct {
    uint8_t        clock_phase;   // 0-7 (Register 0xBA trim bits)
    profile_cal_t  profile[XHD_NUM_TIMING_PROFILES];
} encoder_cal_t;

typedef struct {
    uint32_t       magic;             // 0   : "XHD4" (0x58484434)
    uint8_t        color_space;       // 4   : 0=Auto, 1=Full, 2=Limited, 3=YCbCr, 4=RGB pass, 5=Auto SD-Full
    uint8_t        aspect_ratio;      // 5   : 0=Auto, 1=16:9, 2=4:3
    uint8_t        scan_mode;         // 6   : 0=Auto, 1=TV, 2=PC
    uint8_t        active_encoder;    // 7   : 0=Xcalibur, 1=Conexant, 2=Focus
    encoder_cal_t  enc_cal[3];        // 8   : [0]=Xcalibur, [1]=Conexant, [2]=Focus (3 * 73 = 219 B)
    uint8_t        wire_vic_mode;     // 227 : 0=Honest, 1=Chip-Auto
    uint8_t        cec_clock_off;     // 228 : 0=On (12MHz generated), 1=Off
    uint8_t        cec_one_touch_off; // 229 : 0=On (wake & switch), 1=Off
    uint8_t        smbus_freeze;      // 230 : 0=Normal, 1=Freeze mode
    uint8_t        cec_standby_off;   // 231 : 0=On (standby at shutdown), 1=Off
    uint8_t        reserved[2];       // 232
    uint32_t       checksum;          // 234 : Checksum over bytes 0..233
} xhd_user_config;

// --- SPD InfoFrame Block (28 Bytes on Page 63 immediately after user_config) ---
typedef struct {
    uint8_t   name[XHD_SPD_NAME_LEN]; // 0..7 Vendor, 8..23 Product
    uint32_t  checksum;
} xhd_spd_config;

#pragma pack(pop)

#define XHD_CONFIG_MAGIC                0x58484434 // "XHD4"
