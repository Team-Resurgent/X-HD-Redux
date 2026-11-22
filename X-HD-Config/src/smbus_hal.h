// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "fw_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

// One byte from / to any SMBus device (8-bit address, as the kernel takes it). False on a
// NACK or bus error. These match fw_io_t's read/write, so the firmware updater uses them too.
bool smbus_read_at(uint8_t addr, uint8_t cmd, uint8_t *val);
bool smbus_write_at(uint8_t addr, uint8_t cmd, uint8_t val);

// The same at the X-HD (XHD_SMBUS_ADDR).
bool smbus_read_byte(uint8_t opcode, uint8_t *outVal);
bool smbus_write_byte(uint8_t opcode, uint8_t val);

// Which board answers: the X-HD application, an X-HD in update mode (its bootloader also
// answers the version opcodes), a stock HD+, or nothing. Tab 8 uses the same detection
// (fw_detect), so the two never disagree. Only FW_BOARD_XHD_APP answers the settings opcodes.
bool smbus_detect(fw_board_t *out);
bool smbus_read_video_info(XHD_VIDEO_INFO *outInfo);
bool smbus_read_telemetry(XHD_TELEMETRY *outTlm);
bool smbus_read_about(XHD_ABOUT *outAbout);
bool smbus_read_user_config(xhd_user_config *outCfg);
bool smbus_read_spd(char outSpd[25]);
bool smbus_write_spd(const char spd[24]);
bool smbus_read_edid(uint8_t outEdid[256]);
// The picture profiles (XHD_PICTURE_SET). read() is false when the board doesn't answer or
// sends out-of-range fields -- firmware from before the v2 picture controls. read_live() reads
// just byte 0, the profile the board uses now. write() stages one profile, applies it (live
// if it's the one in use) and reads everything back into *now (may be NULL): true only when
// the board holds exactly what was sent.
bool smbus_read_picture(XHD_PICTURE_SET *out);
bool smbus_read_picture_live(uint8_t *live);
bool smbus_write_picture(uint8_t profile, const XHD_PICTURE *pic, XHD_PICTURE_SET *now);

// Live register setters
bool smbus_set_color_space(uint8_t cs);
bool smbus_set_aspect_ratio(uint8_t ar);
bool smbus_set_scan_mode(uint8_t sm);
bool smbus_set_wire_vic_mode(uint8_t wvm);
bool smbus_set_clock_phase(uint8_t phase);
bool smbus_set_position_h(int8_t h);
bool smbus_set_position_v(int8_t v);
bool smbus_set_cec_clock(bool enable);
bool smbus_set_cec_one_touch(bool enable);
// mode: 0 = restore the TV at shutdown (default), 1 = do nothing, 2 = always standby
// (the config struct's cec_standby_off values; the wire swaps 0 and 1 for back-compat).
bool smbus_set_cec_standby(uint8_t mode);
bool smbus_set_sd_exact(bool exact);
bool smbus_set_audio_mute(bool mute);
bool smbus_set_freeze(bool freeze);

// Actions (save flash, reload, defaults, reapply)
bool smbus_trigger_action(uint8_t action);
bool smbus_read_live_flags(uint8_t *outFlags);
bool smbus_read_cec_status(uint8_t *outStatus);
bool smbus_poll_cec_frame(uint8_t outFrame[16], uint8_t *outLen);

#ifdef __cplusplus
}
#endif
