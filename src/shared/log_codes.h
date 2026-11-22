// SPDX-License-Identifier: GPL-3.0-only
// Log lines as codes: the firmware sends "~XX" and the arguments, and the comment on each
// line is the text it stands for (tools/xhd.py log expands them). Never reuse or renumber a
// code: lines from older firmware must still decode.
#ifndef __LOG_CODES_H__
#define __LOG_CODES_H__

#define LOG_BOOT_IMAGE_CRC_OK        "~01\t%X\n"   // [BOOT] image CRC ok (%08lX)
#define LOG_BOOT_IMAGE_CRC_BAD       "~02\t%X\t%X\n"   // [BOOT] image CRC BAD: header %08lX, flash %08lX
#define LOG_BOOT_IMAGE_NO_CRC        "~03\n"   // [BOOT] image has no CRC header
#define LOG_BOOT_NO_MODE_TABLES      "~04\t%X\n"   // [BOOT] no mode tables in the bootloader (%08lX)
#define LOG_ADV_WAIT_MS              "~05\t%d\n"   // [ADV WAIT] %ldms
#define LOG_SMBUS_SLAVE_READY        "~06\t%X\n"   // SMBus: slave %02X ready
#define LOG_SMBUS_FROZEN             "~07\t%X\n"   // SMBus: IGNORED (frozen) mode=%08X
#define LOG_SMBUS_PACKET             "~08\t%X\t%X\t%X\t%X\t%X\n"   // SMBus: encoder=%02X region=%02X mode=%08X title=%08X avinfo=%08X
#define LOG_SMBUS_APPLY_BAD          "~09\t%X\n"   // SMBus: CONFIG_APPLY bad value=%02X
#define LOG_SMBUS_COLOR              "~0A\t%u\n"   // SMBus: color %u
#define LOG_ENCODER_PROBE_CONFIRMED  "~0B\t%s\n"   // [ENCODER PROBE] confirmed %s
#define LOG_HDMI_READY               "~0C\t%u\t%u\n"   // [HDMI] HPD+MSEN ready (up at %lums, setup %lums)
#define LOG_HDMI_HPD_MSEN            "~0D\t%s\t%s\t%X\n"   // [HDMI] HPD: %s, MSEN: %s (0x42=0x%02X)
#define LOG_HDMI_DISCONNECTED        "~0E\n"   // [HDMI] disconnected
#define LOG_ENCODER_PROBE_RESOLVED   "~10\n"   // [ENCODER PROBE] resolved
#define LOG_ENCODER_PROBE_UNKNOWN    "~11\t%s\n"   // [ENCODER PROBE] %s cap, unknown
#define LOG_ENCFMT                   "~12\t%s\n"   // [ENCFMT] encoder input -> %s
#define LOG_RECOVERY_ENCODER         "~13\t%s\n"   // [RECOVERY] encoder cycle -> %s
#define LOG_PLL_LOCKED               "~14\n"   // [PLL EVENT] GPU Pixel Clock LOCKED
#define LOG_PLL_LOST                 "~15\t%X\n"   // [PLL EVENT] GPU Pixel Clock LOST (9E=%02X)
#define LOG_EDID_RETRY_OK            "~16\n"   // [EDID] retry ok
#define LOG_AUDIO_SMC_CLAMP_MUTED    "~17\n"   // [AUDIO] SMC clamp: muted
#define LOG_BIOS_BAD_IDX             "~18\t%X\n"   // [BIOS] bad idx %02X
#define LOG_BIOS_RINGS               "~19\n"   // [BIOS] By the rings...
#define LOG_BIOS_APPLY               "~1A\t%X\t%X\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n"   // (rendered by _bios_apply)
#define LOG_CEC_MSG                  "~1B\t%s\t%s\t%s\n"   // [CEC] %s%s%s
#define LOG_CEC_ACTIVE_SOURCE        "~1C\t%u\n"   // [CEC] active source: %u
#define LOG_CEC_RX_DROPPED           "~1D\n"   // [CEC] rx dropped
#define LOG_CEC_ARB_LOST_GIVING      "~1E\n"   // [CEC] arb lost, giving up
#define LOG_CEC_ARB_LOST_RETRYING    "~1F\n"   // [CEC] arb lost, retrying 1/s
#define LOG_CEC_STANDBY_SKIPPED      "~20\n"   // [CEC] standby skipped
#define LOG_EDID_BLK0_READ_FAILED    "~21\n"   // [EDID] blk0 read failed
#define LOG_EDID_BLK0_BAD_HEADER     "~22\t%X\t%X\n"   // [EDID] blk0 bad header (C8=%02X 96=%02X)
#define LOG_EDID_BLK0_CSUM           "~23\t%X\n"   // [EDID] blk0 csum %02X
#define LOG_EDID_EXT_READ_FAILED     "~24\n"   // [EDID] ext read failed
#define LOG_EDID_EXT_CSUM            "~25\t%X\n"   // [EDID] ext csum %02X
#define LOG_EDID_FALLBACK            "~26\n"   // [EDID] read failed, DVI/RGB fallback
#define LOG_I2C_INIT_FAILED          "~27\t%s\n"   // %s I2C init failed
#define LOG_TMDS_CYCLE               "~28\t%u\n"   // [TMDS] cycle %lums after previous
#define LOG_ADV_WIRE_VIC             "~29\t%u\n"   // [ADV] wire VIC %u
#define LOG_VIC_SENT                 "~2A\t%u\t%X\n"   // Actual VIC Sent : VIC %u (0x%02X)
#define LOG_PROBE_ARMED              "~2B\n"   // [PROBE] armed
#define LOG_MODE                     "~2C\t%X\t%s\n"   // [MODE] %02X %s
#define LOG_TMDS_STATE               "~2D\t%c\n"   // [TMDS] %c
#define LOG_CONFIG_SAVE_FAILED       "~2E\n"   // [CONFIG] save failed
#define LOG_CONFIG_LOADED            "~2F\t%u\t%s\n"   // [CONFIG] loaded (phase %u, %s)
#define LOG_CONFIG_DEFAULTS          "~30\n"   // [CONFIG] defaults (none saved)
#define LOG_CONFIG_SELF_LATCHED      "~31\t%s\n"   // [CONFIG] self-latched %s
#define LOG_ARB_WORT                 "~33\n"   // [ARB] Wort Wort Wort.
#define LOG_ARB_L6_STUCK             "~34\t%u\t%X\t%X\t%u\n"   // [ARB] L6 hold stuck %lums -- released (hw=0x%02X ac=0x%02X pclk=%lu)
#define LOG_ARB_RELOCK_REAPPLY       "~36\t%u\n"   // [ARB] relock reapply int VIC %u
#define LOG_ARB_BAD_TIMING           "~38\n"   // [ARB] bad timing
#define LOG_ADV_IN_PLACE             "~39\n"   // [ADV] in-place, raster unchanged
#define LOG_ADV_SETTINGS             "~3A\n"   // [ADV] settings in place
#define LOG_AUTODETECT               "~3B\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n"   // (rendered by _autodetect)
#define LOG_PIC_PROFILE              "~3C\t%u\n"   // [PIC] profile %u
#define LOG_ARB_PICK                 "~3D\t%d\t%u\t%X\t%X\t%u\t%u\t%u\t%u\t%u\n"   // (rendered by _arb_pick)
#define LOG_HDMI_BACK                "~3E\t%u\n"   // (rendered by _hdmi_back)

#endif // __LOG_CODES_H__
