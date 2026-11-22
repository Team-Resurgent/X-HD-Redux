// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>
#include "smbus_hal.h"

_Static_assert(sizeof(XHD_PICTURE) == 14, "one picture profile is 14 bytes");
_Static_assert(sizeof(XHD_PICTURE_SET) == 43, "READ_PICTURE is 43 bytes");

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

bool smbus_read_at(uint8_t addr, uint8_t cmd, uint8_t *val)
{
    if (!val) {
        return false;
    }
    DWORD v = 0;
    if (!NT_SUCCESS(HalReadSMBusValue(addr, cmd, FALSE, &v))) {
        return false;
    }
    *val = (uint8_t)(v & 0xFF);
    return true;
}

bool smbus_write_at(uint8_t addr, uint8_t cmd, uint8_t val)
{
    return NT_SUCCESS(HalWriteSMBusValue(addr, cmd, FALSE, (DWORD)val));
}

bool smbus_read_byte(uint8_t opcode, uint8_t *outVal)
{
    return smbus_read_at(XHD_SMBUS_ADDR, opcode, outVal);
}

bool smbus_write_byte(uint8_t opcode, uint8_t val)
{
    return smbus_write_at(XHD_SMBUS_ADDR, opcode, val);
}

// fw_detect() only reads; the rest of fw_io_t is never called from it.
static void detect_sleep(uint32_t ms) { Sleep(ms); }
static uint32_t detect_ticks(void) { return GetTickCount(); }
static void detect_progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
static void detect_log(const char *msg) { (void)msg; }

bool smbus_detect(fw_board_t *out)
{
    static const fw_io_t kIo = { smbus_read_at, smbus_write_at, detect_sleep, detect_ticks,
                                 detect_progress, detect_log };
    return fw_detect(&kIo, out);
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

static bool picture_valid(const XHD_PICTURE *p)
{
    if (p->white_clip < XHD_PIC_WHITE_MIN || p->black_clip > XHD_PIC_BLACK_MAX ||
        p->contrast < XHD_PIC_CONTRAST_MIN || p->contrast > XHD_PIC_CONTRAST_MAX ||
        p->brightness < -XHD_PIC_BRIGHT_MAX || p->brightness > XHD_PIC_BRIGHT_MAX ||
        p->chroma_cos < 0 || p->chroma_cos > 2 * XHD_PIC_Q12_ONE ||
        p->chroma_sin < -2 * XHD_PIC_Q12_ONE || p->chroma_sin > 2 * XHD_PIC_Q12_ONE) {
        return false;
    }
    for (int r = 0; r < 3; r++) {
        if (p->gain[r] < XHD_PIC_GAIN_MIN || p->gain[r] > XHD_PIC_GAIN_MAX ||
            p->bias[r] < -XHD_PIC_BIAS_MAX || p->bias[r] > XHD_PIC_BIAS_MAX) {
            return false;
        }
    }
    return true;
}

// Byte 0 is the live profile (0-2). v1 firmware sent its 8-byte block there (white clip
// first, 128-255), so it fails here like firmware without picture controls.
bool smbus_read_picture(XHD_PICTURE_SET *out)
{
    if (!out) return false;
    // APPLY 0 resets both cursors without applying anything.
    if (!smbus_write_byte(XHD_OP_WRITE_PICTURE_APPLY, 0)) return false;
    uint8_t *b = (uint8_t *)out;
    for (size_t i = 0; i < sizeof(*out); i++) {
        if (!smbus_read_byte(XHD_OP_READ_PICTURE, &b[i])) return false;
    }
    if (out->live >= XHD_PIC_PROFILES) return false;
    for (int i = 0; i < XHD_PIC_PROFILES; i++) {
        if (!picture_valid(&out->prof[i])) return false;
    }
    return true;
}

bool smbus_read_picture_live(uint8_t *live)
{
    uint8_t b;
    if (!live || !smbus_write_byte(XHD_OP_WRITE_PICTURE_APPLY, 0) ||
        !smbus_read_byte(XHD_OP_READ_PICTURE, &b) || b >= XHD_PIC_PROFILES) {
        return false;
    }
    *live = b;
    return true;
}

bool smbus_write_picture(uint8_t profile, const XHD_PICTURE *pic, XHD_PICTURE_SET *now)
{
    if (!pic || profile >= XHD_PIC_PROFILES || !picture_valid(pic)) return false;
    if (!smbus_write_byte(XHD_OP_WRITE_PICTURE_APPLY, 0)) return false;
    if (!smbus_write_byte(XHD_OP_WRITE_PICTURE_DATA, profile)) return false;
    const uint8_t *b = (const uint8_t *)pic;
    for (size_t i = 0; i < sizeof(*pic); i++) {
        if (!smbus_write_byte(XHD_OP_WRITE_PICTURE_DATA, b[i])) return false;
    }
    if (!smbus_write_byte(XHD_OP_WRITE_PICTURE_APPLY, 1)) return false;
    // The board rejects a partial or out-of-range block silently: check what it holds.
    XHD_PICTURE_SET back;
    if (!smbus_read_picture(&back)) return false;
    if (now) *now = back;
    return memcmp(&back.prof[profile], pic, sizeof(*pic)) == 0;
}

// Read back: firmware older than a value (6, YCbCr through the CSC) ignores it and keeps its
// setting, which READ_COLOR_SPACE then still shows.
bool smbus_set_color_space(uint8_t cs)
{
    uint8_t back;
    return smbus_write_byte(XHD_OP_WRITE_COLOR_SPACE, cs) &&
           smbus_read_byte(XHD_OP_READ_COLOR_SPACE, &back) && back == cs;
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

bool smbus_set_cec_standby(uint8_t mode)
{
    // Wire values: 1 = restore (the pre-mode "on"), 0 = nothing, 2 = always standby.
    return smbus_write_byte(XHD_OP_WRITE_CEC_STANDBY, mode == 0 ? 1 : (mode == 1 ? 0 : 2));
}

bool smbus_set_sd_exact(bool exact)
{
    return smbus_write_byte(XHD_OP_WRITE_SD_EXACT, exact ? 1 : 0);
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

    // A byte lost to a bus error drops the whole frame: a key with a wrong opcode or operand
    // would be acted on as if the TV had sent it.
    bool ok = true;
    for (uint8_t i = 0; i < len && ok; i++) {
        ok = smbus_read_byte(XHD_OP_READ_CEC_RX_DATA, &outFrame[i]);
    }

    // Acknowledge and release the hardware buffer either way
    smbus_write_byte(XHD_OP_WRITE_CEC_RX_ACK, 1);
    if (!ok) {
        return false;
    }
    *outLen = len;
    return true;
}
