#pragma once

#include "xbox_base.h"
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

// Basic SMBus byte operations
bool smbus_read_byte(uint8_t opcode, uint8_t *outVal);
bool smbus_write_byte(uint8_t opcode, uint8_t val);

// High-level X-HD commands
bool smbus_detect_board(uint8_t *outMode, uint8_t ver[4]);
bool smbus_read_video_info(XHD_VIDEO_INFO *outInfo);
bool smbus_read_telemetry(XHD_TELEMETRY *outTlm);
bool smbus_read_about(XHD_ABOUT *outAbout);
bool smbus_read_user_config(xhd_user_config *outCfg);
bool smbus_read_spd(char outSpd[25]);
bool smbus_write_spd(const char spd[24]);
bool smbus_read_edid(uint8_t outEdid[256]);

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
bool smbus_set_cec_standby(bool enable);
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
