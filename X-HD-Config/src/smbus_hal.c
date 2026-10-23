#include <string.h>
#include "smbus_hal.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

bool smbus_read_byte(uint8_t opcode, uint8_t *outVal)
{
    if (!outVal) {
        return false;
    }
    DWORD val = 0;
    NTSTATUS st = HalReadSMBusValue(XHD_SMBUS_ADDR, opcode, FALSE, &val);
    if (!NT_SUCCESS(st)) {
        return false;
    }
    *outVal = (uint8_t)(val & 0xFF);
    return true;
}

bool smbus_write_byte(uint8_t opcode, uint8_t val)
{
    NTSTATUS st = HalWriteSMBusValue(XHD_SMBUS_ADDR, opcode, FALSE, (DWORD)val);
    return NT_SUCCESS(st);
}

bool smbus_detect_board(uint8_t *outMode, uint8_t ver[4])
{
    uint8_t mode = 0;
    if (!smbus_read_byte(XHD_OP_READ_MODE, &mode)) {
        return false;
    }

    if (outMode) {
        *outMode = mode;
    }

    if (ver) {
        smbus_read_byte(XHD_OP_READ_VERSION1, &ver[0]);
        smbus_read_byte(XHD_OP_READ_VERSION2, &ver[1]);
        smbus_read_byte(XHD_OP_READ_VERSION3, &ver[2]);
        smbus_read_byte(XHD_OP_READ_VERSION4, &ver[3]);
        // 0xFF 0xFF 0xFF 0xFF means not an X-HD board
        if (ver[0] == 0xFF && ver[1] == 0xFF && ver[2] == 0xFF && ver[3] == 0xFF) {
            return false;
        }
    }

    return true;
}

bool smbus_read_video_info(XHD_VIDEO_INFO *outInfo)
{
    if (!outInfo) return false;

    // Latch snapshot and reset cursor
    if (!smbus_write_byte(XHD_OP_WRITE_VIDEO_INFO_LATCH, 1)) {
        return false;
    }

    XHD_VIDEO_INFO vi;
    memset(&vi, 0, sizeof(vi));
    uint8_t *raw = (uint8_t *)&vi;
    for (int i = 0; i < (int)sizeof(XHD_VIDEO_INFO); i++) {
        if (!smbus_read_byte(XHD_OP_READ_VIDEO_INFO, &raw[i])) {
            return false;
        }
    }

    if (vi.struct_version != 1 || vi.struct_size != sizeof(XHD_VIDEO_INFO)) {
        return false;
    }
    // With no mode applied the mode fields aren't valid and aren't checked; callers test
    // XHD_VI_FLAG_APPLIED before showing them.
    if ((vi.flags & XHD_VI_FLAG_APPLIED) &&
        (vi.width < 320 || vi.width > 1920 || vi.height < 240 || vi.height > 1080 ||
         vi.color_space > 5)) {
        return false;
    }

    *outInfo = vi;
    return true;
}

bool smbus_read_telemetry(XHD_TELEMETRY *outTlm)
{
    if (!outTlm) return false;

    // Reset telemetry cursor to 0
    if (!smbus_write_byte(XHD_OP_WRITE_TELEMETRY_INDEX, 0)) {
        return false;
    }

    XHD_TELEMETRY tlm;
    memset(&tlm, 0, sizeof(tlm));
    uint8_t *raw = (uint8_t *)&tlm;
    for (int i = 0; i < (int)sizeof(XHD_TELEMETRY); i++) {
        if (!smbus_read_byte(XHD_OP_READ_TELEMETRY, &raw[i])) {
            return false;
        }
    }

    if (tlm.struct_version != 1 || tlm.struct_size != sizeof(XHD_TELEMETRY)) {
        return false;
    }

    *outTlm = tlm;
    return true;
}

bool smbus_read_about(XHD_ABOUT *outAbout)
{
    if (!outAbout) return false;

    // Reset about cursor to 0
    if (!smbus_write_byte(XHD_OP_WRITE_ABOUT_INDEX, 0)) {
        return false;
    }

    XHD_ABOUT ab;
    memset(&ab, 0, sizeof(ab));
    uint8_t *raw = (uint8_t *)&ab;
    for (int i = 0; i < (int)sizeof(XHD_ABOUT); i++) {
        if (!smbus_read_byte(XHD_OP_READ_ABOUT, &raw[i])) {
            return false;
        }
    }

    if (ab.struct_version != 1 || ab.struct_size != sizeof(XHD_ABOUT)) {
        return false;
    }

    // Ensure strings are null-terminated
    ab.git[sizeof(ab.git) - 1] = '\0';
    ab.built[sizeof(ab.built) - 1] = '\0';

    *outAbout = ab;
    return true;
}

bool smbus_read_user_config(xhd_user_config *outCfg)
{
    if (!outCfg) return false;

    // Position cursor at byte 0
    if (!smbus_write_byte(XHD_OP_WRITE_USER_CONFIG_INDEX, 0)) {
        return false;
    }

    xhd_user_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    uint8_t *raw = (uint8_t *)&cfg;
    for (int i = 0; i < (int)sizeof(xhd_user_config); i++) {
        if (!smbus_read_byte(XHD_OP_READ_USER_CONFIG, &raw[i])) {
            return false;
        }
    }

    if (cfg.magic != XHD_CONFIG_MAGIC) {
        return false;
    }

    *outCfg = cfg;
    return true;
}

bool smbus_read_spd(char outSpd[25])
{
    if (!outSpd) return false;

    // Reset cursor
    if (!smbus_write_byte(XHD_OP_WRITE_SPD_APPLY, 0)) {
        return false;
    }

    for (int i = 0; i < 24; i++) {
        uint8_t b = 0;
        if (!smbus_read_byte(XHD_OP_READ_SPD_DATA, &b)) {
            return false;
        }
        outSpd[i] = (char)b;
    }
    outSpd[24] = '\0';
    return true;
}

bool smbus_write_spd(const char spd[24])
{
    if (!spd) return false;

    // Reset staging cursor
    if (!smbus_write_byte(XHD_OP_WRITE_SPD_APPLY, 0)) {
        return false;
    }

    for (int i = 0; i < 24; i++) {
        if (!smbus_write_byte(XHD_OP_WRITE_SPD_DATA, (uint8_t)spd[i])) {
            return false;
        }
    }

    // Commit staging and resend InfoFrame
    return smbus_write_byte(XHD_OP_WRITE_SPD_APPLY, 1);
}

bool smbus_set_color_space(uint8_t cs)
{
    return smbus_write_byte(XHD_OP_WRITE_COLOR_SPACE, cs);
}

bool smbus_set_aspect_ratio(uint8_t ar)
{
    return smbus_write_byte(XHD_OP_WRITE_ASPECT_RATIO, ar);
}

bool smbus_set_scan_mode(uint8_t sm)
{
    return smbus_write_byte(XHD_OP_WRITE_SCAN_MODE, sm);
}

bool smbus_set_wire_vic_mode(uint8_t wvm)
{
    return smbus_write_byte(XHD_OP_WRITE_WIRE_VIC_MODE, wvm);
}

bool smbus_set_clock_phase(uint8_t phase)
{
    return smbus_write_byte(XHD_OP_WRITE_PHASE, phase);
}

bool smbus_set_position_h(int8_t h)
{
    return smbus_write_byte(XHD_OP_WRITE_POSITION_H, (uint8_t)h);
}

bool smbus_set_position_v(int8_t v)
{
    return smbus_write_byte(XHD_OP_WRITE_POSITION_V, (uint8_t)v);
}

bool smbus_set_cec_clock(bool enable)
{
    return smbus_write_byte(XHD_OP_WRITE_CEC_CLOCK_OUT, enable ? 1 : 0);
}

bool smbus_set_cec_one_touch(bool enable)
{
    return smbus_write_byte(XHD_OP_WRITE_CEC_ONE_TOUCH, enable ? 1 : 0);
}

bool smbus_set_cec_standby(bool enable)
{
    return smbus_write_byte(XHD_OP_WRITE_CEC_STANDBY, enable ? 1 : 0);
}

bool smbus_set_audio_mute(bool mute)
{
    return smbus_write_byte(XHD_OP_WRITE_AUDIO_MUTE, mute ? 1 : 0);
}

bool smbus_set_freeze(bool freeze)
{
    return smbus_write_byte(XHD_OP_WRITE_FREEZE, freeze ? 1 : 0);
}

bool smbus_trigger_action(uint8_t action)
{
    return smbus_write_byte(XHD_OP_WRITE_CONFIG_ACTION, action);
}

bool smbus_read_live_flags(uint8_t *outFlags)
{
    if (!outFlags) return false;
    return smbus_read_byte(XHD_OP_READ_LIVE_FLAGS, outFlags);
}

bool smbus_read_cec_status(uint8_t *outStatus)
{
    if (!outStatus) return false;
    return smbus_read_byte(XHD_OP_READ_CEC_STATUS, outStatus);
}

bool smbus_read_edid(uint8_t outEdid[256])
{
    if (!outEdid) return false;

    // Reset EDID read cursor to byte index 0
    if (!smbus_write_byte(XHD_OP_WRITE_EDID_INDEX, 0)) {
        return false;
    }

    for (int i = 0; i < 256; i++) {
        uint8_t b = 0;
        if (!smbus_read_byte(XHD_OP_READ_EDID, &b)) {
            return false;
        }
        outEdid[i] = b;
    }

    return true;
}

bool smbus_poll_cec_frame(uint8_t outFrame[16], uint8_t *outLen)
{
    if (!outFrame || !outLen) return false;

    uint8_t st = 0;
    if (!smbus_read_byte(XHD_OP_READ_CEC_STATUS, &st)) {
        return false;
    }

    // Bit 4: XHD_CEC_STATUS_RX_PENDING (0x10)
    if (!(st & XHD_CEC_STATUS_RX_PENDING)) {
        return false;
    }

    uint8_t len = 0;
    if (!smbus_read_byte(XHD_OP_READ_CEC_RX_LEN, &len) || len == 0 || len > 16) {
        smbus_write_byte(XHD_OP_WRITE_CEC_RX_ACK, 1);
        return false;
    }

    for (uint8_t i = 0; i < len; i++) {
        uint8_t b = 0;
        if (smbus_read_byte(XHD_OP_READ_CEC_RX_DATA, &b)) {
            outFrame[i] = b;
        } else {
            outFrame[i] = 0;
        }
    }

    // Acknowledge and release the hardware buffer
    smbus_write_byte(XHD_OP_WRITE_CEC_RX_ACK, 1);
    *outLen = len;
    return true;
}
