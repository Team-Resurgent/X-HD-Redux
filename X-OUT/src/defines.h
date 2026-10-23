#pragma once

#define WIDTH 512
#define LINE_HEIGHT 13
#define MAX_HEIGHT 32

#define OUT_X 16
#define OUT_Y 16

#define PITCH (WIDTH * 2)

// X-HD SMBus slave address and opcodes
#define XHD_SMBUS_ADDR 0xD2
#define I2C_HDMI_COMMAND_READ_MODE 5
#define I2C_HDMI_COMMAND_READ_VIDEO_INFO 22
#define I2C_HDMI_COMMAND_WRITE_VIDEO_INFO_LATCH 151

#pragma pack(push, 1)
typedef struct {
    unsigned char  struct_version;      // 0  : 1
    unsigned char  struct_size;         // 1  : 24
    unsigned char  change_count;        // 2
    unsigned char  flags;               // 3  : XHD_VI_*
    unsigned short width;               // 4  : applied raster width
    unsigned short height;              // 6  : applied raster height
    unsigned long  pixel_clock_hz;      // 8  : measured input clock
    unsigned short refresh_mhz;         // 12 : exact refresh in mHz (59940 = 59.94 Hz)
    unsigned char  wire_vic;            // 14 : wire VIC told to the sink
    unsigned char  internal_vic;        // 15 : internal CEA family
    unsigned char  detected_vic;        // 16 : line detector VIC
    unsigned char  console_mode_index;  // 17 : BIOS mode index
    unsigned char  color_space;         // 18 : 1 RGB full, 2 RGB limited, 3 YCbCr, 4 RGB pass
    char           pos_h;               // 19
    char           pos_v;               // 20
    unsigned char  reserved[3];         // 21
} XHD_VIDEO_INFO;
#pragma pack(pop)

#define XHD_VI_WIDESCREEN 0x02   // AVI aspect 16:9
#define XHD_VI_APPLIED    0x20   // a mode is applied (the mode fields are valid)

#define NV_PVIDEO 0x8000

#define NV_PRAMDAC 0x00680000
#define NV_PRAMDAC_FP_VDISPLAY_END 0x800
#define NV_PRAMDAC_FP_VTOTAL 0x804
#define NV_PRAMDAC_FP_HDISPLAY_END 0x820

#define NV_PVIDEO_INTR 0x100
#define NV_PVIDEO_INTR_BUFFER_0 (1 << 0)
#define NV_PVIDEO_INTR_BUFFER_1 (1 << 4)
#define NV_PVIDEO_INTR_REASON 0x104
#define NV_PVIDEO_INTR_REASON_BUFFER_0 (1 << 0)
#define NV_PVIDEO_INTR_REASON_BUFFER_0_NOTIFICATION 0
#define NV_PVIDEO_INTR_REASON_BUFFER_0_PROTECTION_FAULT 1
#define NV_PVIDEO_INTR_REASON_BUFFER_1 (1 << 4)
#define NV_PVIDEO_INTR_REASON_BUFFER_1_NOTIFICATION 0
#define NV_PVIDEO_INTR_REASON_BUFFER_1_PROTECTION_FAULT 1
#define NV_PVIDEO_INTR_EN 0x140
#define NV_PVIDEO_INTR_EN_BUFFER_0 (1 << 0)
#define NV_PVIDEO_INTR_EN_BUFFER_0_DISABLED 0
#define NV_PVIDEO_INTR_EN_BUFFER_0_ENABLED 1
#define NV_PVIDEO_INTR_EN_BUFFER_1 (1 << 4)
#define NV_PVIDEO_INTR_EN_BUFFER_1_DISABLED 0
#define NV_PVIDEO_INTR_EN_BUFFER_1_ENABLED 1
#define NV_PVIDEO_BUFFER 0x700
#define NV_PVIDEO_BUFFER_0_USE (1 << 0)
#define NV_PVIDEO_BUFFER_1_USE (1 << 4)
#define NV_PVIDEO_STOP 0x704
#define NV_PVIDEO_STOP_OVERLAY (1 << 0)
#define NV_PVIDEO_STOP_OVERLAY_INACTIVE 0
#define NV_PVIDEO_STOP_OVERLAY_ACTIVE 1
#define NV_PVIDEO_STOP_METHOD (1 << 4)
#define NV_PVIDEO_STOP_METHOD_IMMEDIATELY 0
#define NV_PVIDEO_STOP_METHOD_NORMALLY 1

// There's 2 video overlays, so each of the following regs is an array
#define NV_PVIDEO_BASE 0x900 // 64 byte aligned
#define NV_PVIDEO_LIMIT 0x908 // 64 byte aligned
#define NV_PVIDEO_LUMINANCE 0x910
#define NV_PVIDEO_CHROMINANCE 0x918
#define NV_PVIDEO_OFFSET 0x920 // 64 byte aligned
#define NV_PVIDEO_SIZE_IN 0x928
#define NV_PVIDEO_SIZE_IN_WIDTH 0x000007FE
#define NV_PVIDEO_SIZE_IN_HEIGHT 0x03FF0000
#define NV_PVIDEO_POINT_IN 0x930
#define NV_PVIDEO_POINT_IN_S 0x00007FFF
#define NV_PVIDEO_POINT_IN_T 0xFFFE0000
#define NV_PVIDEO_DS_DX 0x938 // Default: 0x100000
#define NV_PVIDEO_DT_DY 0x940 // Default: 0x100000
#define NV_PVIDEO_POINT_OUT 0x948
#define NV_PVIDEO_POINT_OUT_X 0x00000FFF
#define NV_PVIDEO_POINT_OUT_Y 0x0FFF0000
#define NV_PVIDEO_SIZE_OUT 0x950
#define NV_PVIDEO_SIZE_OUT_WIDTH 0x00000FFF
#define NV_PVIDEO_SIZE_OUT_HEIGHT 0x0FFF0000
#define NV_PVIDEO_FORMAT 0x958
#define NV_PVIDEO_FORMAT_PITCH 0x00001FC0
#define NV_PVIDEO_FORMAT_COLOR 0x00030000
#define NV_PVIDEO_FORMAT_COLOR_LE_YB8CR8YA8CB8 0x0
#define NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 0x1
#define NV_PVIDEO_FORMAT_COLOR_LE_EYB8ECR8EYA8ECB8 0x2
#define NV_PVIDEO_FORMAT_COLOR_LE_ECR8EYB8ECB8EYA8 0x3
#define NV_PVIDEO_FORMAT_DISPLAY (1 << 20)
#define NV_PVIDEO_FORMAT_DISPLAY_ALWAYS 0
#define NV_PVIDEO_FORMAT_DISPLAY_COLOR_KEY_EQUAL 1
#define NV_PVIDEO_FORMAT_MATRIX (1 << 24)
#define NV_PVIDEO_FORMAT_MATRIX_ITURBT601 0
#define NV_PVIDEO_FORMAT_MATRIX_ITURBT709 1
#define NV_PVIDEO_FORMAT_FIELD (1 << 28)
#define NV_PVIDEO_FORMAT_FIELD_TOP 0
#define NV_PVIDEO_FORMAT_FIELD_BOTTOM 1

// Color key is shared for both buffers
#define NV_PVIDEO_COLOR_KEY 0xB00