#ifndef __SMBUS_I2C_H__
#define __SMBUS_I2C_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../shared/types.h"
// The passive bus probe (encoder ID, EEPROM and encoder-register sniffing) has its own
// header; included here so smbus_i2c.h's users get it too.
#include "smbus_probe.h"

#pragma pack(1)
typedef struct
{
    uint8_t encoder;
    uint8_t region;
    uint32_t mode;
    uint32_t titleid;
    uint32_t avinfo;
} SMBusSettings;
#pragma pack()

// -------------------- Telemetry (SMBus opcode 15 / UART "tlm") --------------------
// Diagnostic state that no other opcode carries. Wire layout is fixed -- see
// doc/SMBUS.md section 8. NOT packed on purpose: every field is naturally aligned
// at these offsets, so the MCU's stores stay single-instruction/atomic and the
// slave ISR (reading byte-by-byte) can never see a torn multi-byte value. The
// _Static_asserts below pin the layout the doc and xhd_tool.py both assume.
#define XHD_TELEMETRY_VERSION 1

// hw_flags bits
#define XHD_TLM_HPD            0x01
#define XHD_TLM_MSEN           0x02
#define XHD_TLM_TMDS_ON        0x04  // reserved in v1 (always 0)
#define XHD_TLM_VIDEO_UNGATED  0x08
#define XHD_TLM_AUDIO_ON       0x10
#define XHD_TLM_FREEZE         0x20
#define XHD_TLM_PROBE_ARMED    0x40  // reserved in v1 (always 0)
#define XHD_TLM_PLL_LOCK       0x80

typedef struct
{
    uint32_t pixel_clock_hz;      // 0  : measured (N/CTS), 0 while unlocked/re-acquiring
    uint8_t  detected_input_vic;  // 4  : ADV7511 0x3E >> 2 (input VIC; video info has the output VIC)
    uint8_t  arb_source;          // 5  : arb_source_t -- which arbiter level owns the current mode
    uint8_t  arb_target_vic;      // 6  : arbiter's resolved target VIC
    uint8_t  probed_encoder;      // 7  : xbox_encoder enum (0x00 unk / 0x8A Conexant / 0xD4 Focus / 0xE0 Xcalibur)
    uint8_t  hw_flags;            // 8  : XHD_TLM_* bits
    uint8_t  fault_code;          // 9  : reserved, always 0 (no faults are reported yet)
    uint16_t uptime_s;            // 10 : saturates at 0xFFFF
    uint16_t mode_change_count;   // 12 : applied mode changes since boot (saturating)
    uint8_t  pll_loss_count;      // 14 : saturating
    uint8_t  gate_cycle_count;    // 15 : 0xD6 input-gate/TMDS cycles (every mode-apply + every watchdog recovery), saturating
    uint8_t  blind_event_count;   // 16 : line detector went blind, saturating
    uint8_t  struct_version;      // 17 : XHD_TELEMETRY_VERSION
    uint8_t  struct_size;         // 18 : sizeof(XhdTelemetry)
    uint8_t  reserved;            // 19
} XhdTelemetry;

_Static_assert(sizeof(XhdTelemetry) == 20, "telemetry wire layout is 20 bytes (doc/SMBUS.md sec 8)");
_Static_assert(offsetof(XhdTelemetry, uptime_s) == 10, "telemetry uptime_s must be at offset 10");
_Static_assert(offsetof(XhdTelemetry, mode_change_count) == 12, "telemetry mode_change_count must be at offset 12");

typedef enum
{
    XHD_TLM_EV_MODE_CHANGE = 0,
    XHD_TLM_EV_PLL_LOSS,
    XHD_TLM_EV_GATE_CYCLE,
    XHD_TLM_EV_BLIND,
} xhd_telemetry_event_t;

// Saturating +1 on the matching counter. Safe to call from main-loop context.
void smbus_telemetry_event(xhd_telemetry_event_t ev);
// Narrow aliases for the two events fired from shared adv7511_xbox.c via weak externs
// (need plain, non-enum symbols). gate_cycle <- adv7511_tmds_cycle_begin();
// mode_apply <- adv7511_apply_video_timing() (the single real mode-program choke point).
void smbus_telemetry_note_gate_cycle(void);
void smbus_telemetry_note_mode_apply(void);
// Latch the last arbiter decision (called right after decide_target_vic()).
void smbus_note_arbiter_decision(uint8_t arb_source, uint8_t arb_target_vic);
// Refresh the live (non-counter) fields + uptime. Call once per main-loop iteration
// alongside the main loop's other per-iteration publishes.
void smbus_publish_telemetry(uint32_t pixel_clock_hz, uint8_t detected_input_vic,
                             uint8_t hw_flags, uint8_t probed_encoder);

// -------------------- Video info (SMBus opcodes 22 / 151) --------------------
// What is on screen, for an on-screen overlay or a dashboard. Wire layout: doc/SMBUS.md sec 9. Same alignment rules as
// XhdTelemetry (natural alignment, little-endian, versioned). Refreshed by the main loop;
// WRITE 151 latches a copy in the ISR so a multi-byte field can't tear across a read.
#define XHD_VIDEO_INFO_VERSION 1

#define XHD_VI_INTERLACED  0x01  // scan on HDMI is interlaced (1080i)
#define XHD_VI_WIDESCREEN  0x02  // AVI aspect 16:9
#define XHD_VI_ANAMORPHIC  0x04  // 16:9 flagged on a 640-wide raster
#define XHD_VI_BIOS        0x08  // mode owned by a BIOS packet (else standalone autodetect)
#define XHD_VI_HDMI        0x10  // HDMI sink (else DVI)
#define XHD_VI_APPLIED     0x20  // a mode is applied (the applied fields below are valid)
#define XHD_VI_PLL_LOCK    0x40  // input clock locked
#define XHD_VI_WIRE_AUTO   0x80  // wire-VIC mode chip-auto (else honest) -- set with WRITE 152

typedef struct
{
    uint8_t  struct_version;      // 0  : XHD_VIDEO_INFO_VERSION
    uint8_t  struct_size;         // 1  : sizeof(XhdVideoInfo)
    uint8_t  change_count;        // 2  : low byte of the applied-mode change count -- re-read on change
    uint8_t  flags;               // 3  : XHD_VI_*
    uint16_t width;               // 4  : applied raster
    uint16_t height;              // 6  : applied raster (full frame -- 1080 for 1080i)
    uint32_t pixel_clock_hz;      // 8  : measured (settled), 0 while re-acquiring
    uint16_t refresh_mhz;         // 12 : exact refresh in mHz from the measured clock (field rate for 1080i), 0 = unknown
    uint8_t  wire_vic;            // 14 : VIC the sink is told (AVI/0x3C) -- 0 for a non-CEA raster
    uint8_t  internal_vic;        // 15 : CEA family of the applied timing row
    uint8_t  detected_vic;        // 16 : ADV7511 line detector (0x3E), the console's raster as the chip sees it
    uint8_t  console_mode_index;  // 17 : mode index of the console's last BIOS packet, 0xFF = none (standalone)
    uint8_t  color_space;         // 18 : effective adv_color_space (1 RGB full, 2 RGB limited, 3 YCbCr passthrough, 4 RGB passthrough)
    int8_t   pos_h;               // 19 : picture position, horizontal offset (pixels) for the active mode -- set with WRITE 153
    int8_t   pos_v;               // 20 : picture position, vertical offset (lines) -- set with WRITE 154
    uint8_t  reserved[3];         // 21
} XhdVideoInfo;

_Static_assert(sizeof(XhdVideoInfo) == 24, "video info wire layout is 24 bytes");
_Static_assert(offsetof(XhdVideoInfo, pixel_clock_hz) == 8, "video info pixel_clock_hz at offset 8");
_Static_assert(offsetof(XhdVideoInfo, refresh_mhz) == 12, "video info refresh_mhz at offset 12");

// -------------------- About (SMBus opcode 23 / 155) --------------------
// What `v` shows on UART: identity of this board and build. Static after boot.
#define XHD_ABOUT_VERSION 1
typedef struct
{
    uint8_t struct_version;   // 0  : XHD_ABOUT_VERSION
    uint8_t struct_size;      // 1  : sizeof(XhdAbout)
    uint8_t chip_id;          // 2  : ADV7511 0x00 (0 until the chip is first configured)
    uint8_t chip_rev;         // 3  : ADV7511 0xF5
    uint8_t mcu_uid[12];      // 4  : STM32 96-bit unique ID, as in system memory
    char    git[24];          // 16 : git hash of the build, NUL-padded
    char    built[20];        // 40 : build date/time "YYYY-MM-DDTHH:MM:SS", NUL-padded
} XhdAbout;
_Static_assert(sizeof(XhdAbout) == 60, "about block wire layout is 60 bytes");

// Main loop, once the ADV7511 is configured: record its chip ID/revision for the about block.
void smbus_note_chip_id(uint8_t id, uint8_t rev);

// Main loop: refresh the live copy (version, size and change_count are filled in here).
void smbus_publish_video_info(const XhdVideoInfo *vi);
// The live refresh, for the UART extras line.
uint16_t smbus_video_refresh_mhz(void);

void smbus_i2c_init();

bool video_mode_updated();

uint32_t get_video_mode_update_sequence();
void ack_video_mode_update(uint32_t seen_sequence);

const SMBusSettings * getSMBusSettings();

// Set by the ISR when a color-space-only SMBus write lands -- distinct from
// video_mode_updated()'s pending-target mechanism since this isn't a new mode command
// to reconcile against live measurement, just a request to make an already-changed
// preference take visible effect on whatever's currently showing. Checked and cleared
// by bios_loop() every main-loop iteration once locked.
bool consume_color_space_reapply_pending(void);
// True once per committed WRITE_SPD_APPLY -- main loop resends the SPD InfoFrame.
bool smbus_consume_spd_apply_pending(void);

// Control suite (opcode 13 read, 141-145 write). The slave ISR only stashes into user config +
// raises a dirty bit; smbus_consume_settings_changes() (called from bios_loop, main-loop
// context) does every register write / flash save / mode reapply off-ISR.

void smbus_consume_settings_changes(const xbox_encoder xb_encoder);
// Drains a pending WRITE_RAM_APPLY flash write off-ISR -- see that command's own comment
// in smbus_i2c.c. Call once per main-loop iteration, alongside the above.
void smbus_consume_flash_write(void);

void smbus_set_sniff(bool enable);
bool smbus_get_sniff(void);

bool smbus_get_apply_frozen(void);
// UART bridge (debug.c `sb`): run one SMBus read/write command from the main loop.
// False for an opcode the bridge doesn't carry (mode-packet staging/apply, updater RAM/flash).
bool smbus_bridge_read(uint8_t cmd, uint8_t *out);
bool smbus_bridge_write(uint8_t cmd, uint8_t value);
void smbus_poll_sniffer(void);


#endif // __SMBUS_I2C_H__
