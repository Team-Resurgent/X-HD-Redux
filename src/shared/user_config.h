#ifndef __USER_CONFIG_H__
#define __USER_CONFIG_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "adv7511_xbox.h"
#include "types.h"
#include "defines.h"

#define XHD_CONFIG_FLASH_PAGE LAST_FLASH_PAGE
#ifndef XHD_CONFIG_FLASH_ADDR   // overridable only so host tests can point it at a RAM page
#define XHD_CONFIG_FLASH_ADDR (FLASH_START_ADDRESS + (XHD_CONFIG_FLASH_PAGE << FLASH_PAGE_SHIFT))
#endif
// Changing this (or the struct layout) invalidates existing Page 63 data -- it fails
// the checksum check in user_config_load() like ordinary corruption and falls back to
// user_config_restore_defaults().
#define XHD_CONFIG_MAGIC      0x58484434 // "XHD4"

// Number of timing-profile slots: the unit of per-encoder hs/vs calibration. mode_indices
// sharing a timing row share a profile -- {0x01}, {0x02}, {0x07,0x08,0x11}, the six PAL rows,
// {0x09}, {0x0A,0x0B}, {0x0C}, {0x0D,0x0E}, {0x0F,0x14} -- plus the standalone synthetics
// {0x13} and {0x15}. 0x01 and 0x02 differ in width, so a delta tuned for one mustn't move
// the other. Slots 12 and 15 are unused (kept rather than renumbering). The map is
// xbox_video_timing.c's s_mode_index_to_profile; keep its largest entry below this value.
#define XHD_NUM_TIMING_PROFILES 18

// Default clock-phase trim index per encoder. Register 0xBA[7:5] = index << 5;
// index 0..7 maps to roughly -1.2ns .. +1.6ns in 0.4ns steps.
#define XHD_DEFAULT_CLOCK_PHASE_XCALIBUR 0 // -1.2ns
#define XHD_DEFAULT_CLOCK_PHASE_CONEXANT 3 //  0.0ns
#define XHD_DEFAULT_CLOCK_PHASE_FOCUS    2 // -0.4ns

typedef enum {
    ASPECT_RATIO_AUTO = 0,
    ASPECT_RATIO_16_9 = 1,
    ASPECT_RATIO_4_3  = 2
} adv_aspect_ratio;

typedef enum {
    SCAN_AUTO = 0, // Auto EDID-Aware / IT Content Game Mode
    SCAN_TV   = 1, // TV Overscan Mode (0x55[1:0] = 01)
    SCAN_PC   = 2  // PC Underscan / 1:1 Pixel Mapping (0x55[1:0] = 10)
} adv_scan_mode;

// What VIC the HDMI AVI InfoFrame declares (0x3B/0x3C, see adv7511_xbox.c's wire-VIC policy).
typedef enum {
    WIRE_VIC_HONEST = 0, // manual pixel repeat: CEA VIC only when the signal IS that CEA
                         // timing (geometry, scan and clock), else 0 -- same mode, same VIC
    WIRE_VIC_AUTO   = 1  // chip auto-detection (0x3B=0x00): whatever the ADV7511's line
                         // detector reports, which carries history from the previous raster
} adv_wire_vic_mode;

#pragma pack(push, 1)
typedef struct {
    int16_t hs_delay_offset; // HS delay delta in pixels, for one specific timing profile
    int16_t vs_delay_offset; // VS delay delta in lines, for one specific timing profile
} profile_cal_t;

typedef struct {
    uint8_t clock_phase; // 0-7 (Register 0xBA trim bits) -- a real per-encoder hardware
                          // property, not per-mode, so this stays global unlike hs/vs below.
    profile_cal_t profile[XHD_NUM_TIMING_PROFILES]; // per (timing-profile, encoder) hs/vs delta,
                                                     // see xbox_video_timing.c's s_mode_index_to_profile[]
} encoder_cal_t;

typedef struct {
    uint32_t magic;          // XHD_CONFIG_MAGIC ("XHD4")
    uint8_t  color_space;    // 0=Auto, 1=Full, 2=Limited, 3=Passthrough, 4=RGB-Passthrough
    uint8_t  aspect_ratio;   // 0=Auto, 1=16:9, 2=4:3
    uint8_t  scan_mode;      // 0=Auto, 1=TV, 2=PC
    uint8_t  active_encoder; // 0=Xcalibur, 1=Conexant, 2=Focus (0xFF = not yet confirmed)
    encoder_cal_t enc_cal[3];// [0]=Xcalibur, [1]=Conexant, [2]=Focus
    uint8_t  wire_vic_mode;  // adv_wire_vic_mode: 0=honest (default), 1=chip-auto. Carved out
                              // of the former reserved[7] -- same size/offsets, so existing
                              // saved configs (reserved bytes always 0) read as honest.
    // CEC switches, all ON by default -- stored inverted (0 = on) so a config saved before
    // these existed (reserved bytes, always 0) reads as on too. Carved out of reserved[].
    uint8_t  cec_clock_off;     // 0 = drive CEC_CLK (12 MHz) from the MCU on PA8 / pin 29 (wired to
                                 // ADV7511 pin 50: by design on other boards, a repair on one); 1 = off
    uint8_t  cec_one_touch_off; // 0 = CEC One Touch Play once per boot (wake the TV, take its
                                 // input); 1 = off
    uint8_t  smbus_freeze;      // 1 = ignore the console's SMBus mode packets (pure standalone,
                                 // a test setting); 0 = default. Live, and at boot once saved.
    uint8_t  cec_standby_off;   // 0 = CEC <Standby> to the TV when the console shuts down
                                 // (power button or software, not a reboot); 1 = off
    uint8_t  reserved[2];    // Reserved
    uint32_t checksum;       // Verification checksum (must stay the LAST field --
                              // calculate_checksum() hashes sizeof - sizeof(checksum))
} xhd_user_config;

// The layout READ_USER_CONFIG streams; scripts/xhd_virtual.py decodes it at these offsets.
_Static_assert(sizeof(xhd_user_config) == 238, "user config layout changed -- update xhd_virtual.py CFG_*");
_Static_assert(offsetof(xhd_user_config, enc_cal) == 8, "user config layout changed -- update xhd_virtual.py CFG_*");
_Static_assert(offsetof(xhd_user_config, wire_vic_mode) == 227, "user config layout changed -- update xhd_virtual.py CFG_*");

// HDMI SPD InfoFrame names. A separate block stored on page 63 directly
// after xhd_user_config, with its own checksum -- deliberately NOT new xhd_user_config
// fields: growing that struct moves its checksum and would need a magic bump, which wipes
// every board's encoder calibration + self-latch and silently disables the (field-
// unupgradable) bootloader's RECOVERY-jumper wipe, which matches on XHD4. Old firmware and
// old bootloaders read only xhd_user_config and never see this; missing/invalid -> defaults.
#define XHD_SPD_NAME_LEN 24      // [0..7] vendor, [8..23] product: 7-bit ASCII, space-padded
// Factory names, sent when there is no SPD block (every board until its first save).
#define XHD_SPD_DEFAULT  "MSFT    Xbox            "
typedef struct {
    uint8_t  name[XHD_SPD_NAME_LEN];
    uint32_t checksum;           // same hash as xhd_user_config's, over name[] only
} xhd_spd_config;
#pragma pack(pop)

// The config lives at the start of flash page 63 (1 KB); the updater's app-invalid flag
// occupies that page's last 2 bytes (APP_INVALID_FLAG_ADDRESS in defines.h). A struct
// grown past 1022 bytes would have user_config_save() overwrite that flag.
_Static_assert(sizeof(xhd_user_config) + sizeof(xhd_spd_config) <= 1022,
               "xhd_user_config + xhd_spd_config must fit page 63 without touching the app-invalid flag (last 2 bytes)");

void user_config_init(xbox_encoder xb_encoder);
// Live SPD names (XHD_SPD_NAME_LEN bytes, see xhd_spd_config). set() validates 7-bit
// printable ASCII (0x20-0x7E) and rejects the whole name otherwise; persists on save().
const uint8_t *user_config_get_spd(void);
bool user_config_set_spd(const uint8_t *name);
bool user_config_save(void);
bool user_config_load(void);
void user_config_restore_defaults(xbox_encoder xb_encoder);
xhd_user_config* user_config_get(void);
int user_config_get_encoder_index(xbox_encoder xb_encoder);

// Read-only, no I2C/ADV7511 access -- safe to call before init_adv(), to pick
// the correct boot-time xb_encoder default when a self-latched value from a
// prior boot exists. Returns false if flash is unprogrammed/invalid.
bool user_config_peek_active_encoder(xbox_encoder *out_encoder);

// Self-latch: persist a live SMBus-reported (or bus-probed) encoder as the new active
// encoder, so the next boot starts correct even when the compile-time factory default
// doesn't match this console. One binary serves all 3 encoders.
void user_config_set_active_encoder(xbox_encoder xb_encoder);

#endif // __USER_CONFIG_H__
