#include "adv7511_xbox.h"
#include "adv7511_edid.h"
#include "user_config.h"
#include "debug.h"
#include <string.h>

// Defined with s_applied_valid further down; adv7511_reinit_chip() needs it first.
static void invalidate_applied_timing(void);

// Telemetry counter hooks (application/smbus_i2c.c). Weak: the bootloader has no telemetry.
extern void smbus_telemetry_note_gate_cycle(void) __attribute__((weak));
extern void smbus_telemetry_note_mode_apply(void) __attribute__((weak));

// The encoder's input pixel format, as sniffed (application/smbus_probe.c). Weak: NULL in
// the bootloader, where Auto keeps its EDID-driven choice.
extern bool xbox_encoder_input_is_rgb(bool *is_rgb, unsigned char *in_mode_raw) __attribute__((weak));

// Analog Devices "Fixed Registers That Must Be Set" (datasheet Table 14). HPD going low
// powers the chip down and resets many of them, and they "should be set after power up", so
// adv7511_reinit_chip() reapplies them every time, not just at boot.
void adv7511_apply_fixed_calibration(void) {
    adv7511_write_register(0x98, 0x03); // Analog TMDS driver bias current
    adv7511_write_register(0x9A, 0xE0); // Internal clock delay trim
    adv7511_write_register(0x9C, 0x30); // PLL filter charge pump (ultra-low jitter)
    adv7511_write_register(0x9D, 0x61); // Clock divider ratio
    adv7511_write_register(0xA2, 0xA4); // TMDS channel drive current (1000mV p-p into 50 ohm)
    adv7511_write_register(0xA3, 0xA4); // TMDS clock drive current
    adv7511_write_register(0xDE, 0x9C); // TMDS output slew rate control
    adv7511_write_register(0xE0, 0xD0); // Internal PLL lock timing
    adv7511_write_register(0xF9, 0x00); // Fixed -- must be 0x00 per datasheet Table 14
}

// 0x15: [3:0] input RGB/YCbCr 4:4:4, 12-bit bus DDR; [7:4] audio 48 kHz.
// 0x16: [7] output 4:4:4, [5:4] 8-bit video, [3:2] video style 1, [1] DDR rising edge,
// [0] YCbCr. A chip power-cycle resets these along with Table 14, so both are reapplied.
void adv7511_set_video_input_format(void) {
    adv7511_write_register(0x15, 0b00100101);
    adv7511_write_register(0x16, 0b00111011);
}

// CEA-861 Source Product Descriptor InfoFrame: the TV labels the input "Xbox", and Source
// Device Info = Game engages Game Mode / ALLM without a menu change. Static content, so it is
// only resent after a chip power-cycle (HPD low resets packet memory and the 0x40 enables)
// and after a hotplug, in case a different sink was connected. Not on a DVI sink.
void adv7511_init_spd_infoframe(void) {
    if (!edid_is_hdmi_sink()) {
        return;
    }

    // Packet Memory Map offsets 0x00-0x1C (Programming Guide Table 8):
    //   0x00-0x02  SPD Header       -- type 0x83, version 0x01, length 25 (0x19)
    //   0x03       checksum         -- left 0: auto-generated (0x4A[7] is 1 at power-up)
    //   0x04-0x0B  Vendor Name      -- user_config_get_spd()[0..7], default "MSFT"
    //   0x0C-0x1B  Product Descr.   -- user_config_get_spd()[8..23], default "Xbox"
    //                                  (7-bit ASCII, space-padded; validated on set)
    //   0x1C       Source Dev Info  -- 0x08 = Game (CEA-861-D Table 15), fixed: it's what
    //                                  engages sink Game Mode / ALLM, not a naming choice
    uint8_t spd_packet[29] = { 0x83, 0x01, 0x19, 0x00 };
    memcpy(&spd_packet[4], user_config_get_spd(), XHD_SPD_NAME_LEN);
    spd_packet[28] = 0x08;

    // Freeze SPD output on the current buffer while packet memory is rewritten (Packet
    // Memory Map 0x1F[7]=1), then release (=0) so the chip resumes from the new bytes and
    // recomputes the checksum.
    uint8_t hold = 0b10000000;
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_PACKET_MEM_I2C_ADDR, 0x1F, I2C_MEMADD_SIZE_8BIT, &hold, 1, 20);
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_PACKET_MEM_I2C_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT,
                      spd_packet, sizeof(spd_packet), 20);
    uint8_t release = 0x00;
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_PACKET_MEM_I2C_ADDR, 0x1F, I2C_MEMADD_SIZE_8BIT, &release, 1, 20);

    // Enable continuous SPD packet transmission (Main Map 0x40[6]=1).
    adv7511_update_register(0x40, 0b01000000, 0b01000000);
}

// One-time MCU-side setup (I2C1 bring-up); doesn't touch the chip. The chip itself is
// configured by adv7511_reinit_chip() once HPD and Monitor Sense are stable, from the main
// loop, so the UART, SMBus and everything else stay responsive whatever the display does.
void init_adv(void) {
    adv7511_i2c_init();
}

// VIC currently declared on the wire (0x3C, and AVI DB4) -- see the wire-VIC policy
// (honest_wire_vic()) further down.
static uint8_t s_wire_vic = VIC_00_VIC_Unavailable;
// Settled clock s_wire_vic was last decided on (0 = none yet for the applied raster).
static uint32_t s_wire_pclk = 0;
// 0x3B in WIRE_VIC_HONEST mode: manual pixel repeat [6:5]=10, PR PLL x1 [2:1]=00, PR value
// x1 [4:3]=00 -- the transmitted VIC is then 0x3C, not the chip's own detection.
#define ADV_PR_MANUAL_X1 0x40
static bool wire_vic_honest(void) {
    return user_config_get()->wire_vic_mode == WIRE_VIC_HONEST;
}

void adv7511_apply_wire_vic_mode(void) {
    adv7511_write_register(0x3B, wire_vic_honest() ? ADV_PR_MANUAL_X1 : 0x00);
}

// Full chip (re)configuration, run once HPD and Monitor Sense are stable -- never while HPD
// might be low: the chip only powers up with HPD high, and until then writes to 0x00-0x93,
// 0x94-0x97 and 0xAF-0xCC can be reset or ignored.
void adv7511_reinit_chip(adv7511 *encoder, const xbox_encoder xb_encoder) {
    // Initialise the encoder object
    adv7511_struct_init(encoder);

    // Forget what was applied: an apply attempted while HPD was low wrote nothing, so the
    // cache could claim a timing is already in place when it never reached the chip.
    invalidate_applied_timing();

    // Power up, then close the input clock gate first thing. The gate's power-on default
    // (0xD6[0]) is open, so the chip drives TMDS from the incoming clock before any config
    // exists -- a real exposure on a brownout that resets the chip but not the MCU.
    // adv7511_disable_video() mutes audio just before closing the gate.
    adv7511_power_up();
    adv7511_disable_video();

    // [7:6] HPD Control (forced to high)
    // [5] Fixed 0
    // [4] TMDS Clock soft turn on
    // [3:1] Fixed 000
    // [0] Input clock gate kept closed -- this write covers the whole register, so bit 0 must
    // stay set or it would reopen the input path.
    adv7511_write_register(0xD6, 0b11010001);

    // Unmask HPD and Monitor Sense interrupts on INT pin
    adv7511_write_register(0x94, 0b11000000);

    // Mask every INT1 (0x95) source too -- none are used, and a stray one could hold the
    // shared, edge-triggered INT pin low for good.
    adv7511_write_register(0x95, 0x00);

    adv7511_set_video_input_format();

    // Setup xbox encoder specific stuff (Xcalibur uses different settings)
    init_adv_encoder_specific(xb_encoder);

    adv7511_apply_fixed_calibration();

#ifdef BUILD_APPLICATION
    adv7511_cec_init();
#endif

    // [0] Enable DE generation. This is derived from HSYNC,VSYNC for video active framing
    adv7511_update_register(0x17, 0b00000001, 0b00000001);

    // Pixel repetition (clears the default 0x80 Max Mode): manual x1 in honest mode, so the
    // declared VIC is ours (0x3C, see the wire-VIC policy); auto in WIRE_VIC_AUTO mode (the
    // chip's own detection). x1 either way. In honest mode 0x3B also serves as the chip-reset
    // canary for adv7511_read_active_vic(): its power-on value is 0.
    adv7511_apply_wire_vic_mode();
    s_wire_vic = VIC_00_VIC_Unavailable;
    s_wire_pclk = 0;

    adv7511_disable_csc();

    // HDMI/DVI mode register. Starts in DVI mode -- update_avi_infoframe() just below sets
    // [1] from the EDID (edid_is_hdmi_sink(), which may block on a first EDID read), so a
    // DVI sink never sees HDMI signalling in the meantime.
    // [7] HDCP Disabled
    // [6:5] Must be set to default value 00
    // [4] Frame encryption
    // [3:2] Must be set to default value 01
    // [1] HDMI (1) / DVI (0)
    // [0] Must be set to default 0
    adv7511_write_register(0xAF, 0b00000100);

    // [7] Enable General Control Packet CHECK
    adv7511_update_register(0x40, 0b10000000, 0b10000000);

    // Initialize compliant AVI InfoFrame before video output begins
    // to prevent TV from defaulting to RGB and displaying purple/pale screens
    update_avi_infoframe(false, VIC_02_480p_60__4_3);

    // Static SPD InfoFrame ("Xbox" / Game Mode). No-op on a DVI sink.
    adv7511_init_spd_infoframe();

    init_adv_audio();

    // Config complete. Power the TMDS outputs down (data channels and the clock driver,
    // 0xA1[2]; the input PLL still locks without it) and OPEN the input clock gate, so the PLL
    // has a permanently open reference to lock to. With the gate closed, ~40% of cold boots sat
    // with 0x9E at 0x00 -- the PLL's come-up is non-deterministic and had nothing to
    // re-acquire from. Nothing reaches the sink: the outputs stay down until the first mode
    // apply's TMDS cycle. The adv7511_disable_video() above still covers the brownout case.
    adv7511_power_down_tmds();
    adv7511_enable_video();
}

// Encoder-specific bus setup: bus order, DDR alignment, and the saved clock phase (`cal
// phase`, page 63 -- every slot is validated at load). Runs again whenever the encoder is
// (re)identified: cold boot, the passive probe, or a BIOS packet naming a different encoder.
void init_adv_encoder_specific(const xbox_encoder xb_encoder) {
    uint8_t phase = user_config_get()->enc_cal[user_config_get_encoder_index(xb_encoder)].clock_phase;
    // 0x48: [6] bus order, [5] DDR alignment. Xcalibur: normal order, D[35:18] (left aligned);
    // Conexant and Focus (and an unknown encoder): LSB..MSB reversed, D[17:0] (right aligned).
    adv7511_update_register(0x48, 0b01100000,
                            (xb_encoder == ENCODER_XCALIBUR) ? 0b00100000 : 0b01000000);
    // [7:5] clock delay
    adv7511_update_register(0xBA, 0b11100000, (uint8_t)(phase << 5));

    // [3:2] No sync pulse
    adv7511_update_register(0xD0, 0b00001100, 0b00001100);
}

void init_adv_audio() {
    // [19:0] N = 6144 (0x001800), the HDMI-recommended N for 48kHz. The only N write in
    // the firmware: the Xbox APU only emits 48kHz, so N is fixed (paired with
    // adv7511_set_video_input_format()'s 0x15[7:4]=48kHz), and adv7511_get_pixel_clock_hz()
    // relies on it being exactly 6144.
    adv7511_write_register(0x01, 0x00);
    adv7511_write_register(0x02, 0x18);
    adv7511_write_register(0x03, 0x00);

    // [6:4] Set SPDIF audio source
    adv7511_update_register(0x0A, 0b01110000, 0b00010000);

    // [7] SPDIF enable
    adv7511_update_register(0x0B, 0b10000000, 0b10000000);

    // Default to hardware MUTED upon power-up/reset to prevent boot pops/clicks
    adv7511_mute_audio();
}

void adv7511_mute_audio(void) {
    // Stop Audio Sample Packets (0x44[5]): no HDMI audio at all, PCM or compressed. Silent to
    // the sink by itself. The S/PDIF receiver (0x0B[7]) stays on deliberately: it also drives
    // the ACR/CTS that the pixel clock is measured from, and clearing it on every mode change
    // forced a ~500 ms CTS re-acquire each time.
    adv7511_update_register(0x44, 0b00100000, 0b00000000);
}

// There is deliberately no audio-FIFO reset: 0x0D[4] is the top bit of the I2S bit-width
// field (0x0D[4:0], default 24), so toggling it would corrupt the width, and the chip has no
// FIFO-reset register. adv7511_mute_audio() is how audio is stopped before a mode switch.

// The TMDS cycle around a mode apply (BIOS and standalone alike): the outputs stay quiet for
// TMDS_QUIET_MS so the sink's receiver drops its clock lock and re-acquires the new clock
// cleanly. A cycle starting within TMDS_BACK_TO_BACK_MS of the last is logged -- back-to-back
// cycles mean a spurious reapply.
#define TMDS_QUIET_MS          120UL
// Audio comes back after TMDS power-up once the sink has had TMDS_AUDIO_WAIT_MS to lock AND
// the audio clock regeneration is running (CTS non-zero), so audio sample packets never
// precede valid ACR packets; TMDS_AUDIO_CTS_MAX_MS bounds the wait for a source with no audio.
#define TMDS_AUDIO_WAIT_MS     80UL
#define TMDS_AUDIO_CTS_MAX_MS  500UL
#define TMDS_BACK_TO_BACK_MS   750UL

// When the previous cycle powered TMDS back up -- only for the back-to-back diagnostic.
static uint32_t last_tmds_cycle_end = 0;

void adv7511_tmds_cycle_begin(void) {
    if (smbus_telemetry_note_gate_cycle) smbus_telemetry_note_gate_cycle();
    uint32_t since_last = HAL_GetTick() - last_tmds_cycle_end;
    if (last_tmds_cycle_end != 0 && since_last < TMDS_BACK_TO_BACK_MS) {
        debug_log("[TMDS] cycle %lums after previous\r\n", (unsigned long)since_last);
    }

    // Close the input clock gate (0xD6[0]) along with the TMDS outputs, forcing a clean
    // re-lock on the new mode. adv7511_disable_video() mutes audio itself.
    adv7511_disable_video();
    adv7511_power_down_tmds();
}

// Deferred cycle: nothing is gated yet. The next adv7511_apply_video_timing() decides -- an
// aspect-only change on the same raster applies in place (no gate, mute or relock); anything
// else closes the gate right there, before any register is written, so the full cycle is
// unchanged. adv7511_tmds_cycle_end() closes it if no apply came. For plain mode changes only;
// recovery paths (PLL, hotplug, forced reapply) use the eager adv7511_tmds_cycle_begin().
static bool s_cycle_deferred = false;
static bool s_cycle_applied_in_place = false;

void adv7511_tmds_cycle_begin_deferred(void) {
    s_cycle_deferred = true;
}

// Non-blocking state machine for the TMDS mode-apply cycle, advanced one stage at a
// time via adv7511_tmds_cycle_service() (called once per main-loop iteration).
// Callers must not start a new cycle while adv7511_tmds_cycle_busy() is true.
typedef enum {
    TMDS_CYCLE_IDLE = 0,
    TMDS_CYCLE_QUIET_WAIT,
    TMDS_CYCLE_PLL_POLL,
    TMDS_CYCLE_AUDIO_WAIT,
} tmds_cycle_stage_t;

static tmds_cycle_stage_t s_cycle_stage = TMDS_CYCLE_IDLE;
static uint32_t s_cycle_stage_start = 0;
static uint32_t s_cycle_next_poll = 0;
static uint8_t s_cycle_poll_attempts = 0;

void adv7511_tmds_cycle_end(void) {
    if (s_cycle_deferred) {
        s_cycle_deferred = false;
        adv7511_tmds_cycle_begin();
    }
    if (s_cycle_applied_in_place) {
        s_cycle_applied_in_place = false;
        return;
    }
    s_cycle_stage_start = HAL_GetTick();
    s_cycle_stage = TMDS_CYCLE_QUIET_WAIT;
}

bool adv7511_tmds_cycle_busy(void) {
    return s_cycle_stage != TMDS_CYCLE_IDLE;
}

// Brings output back after the TMDS drivers were left down without a full cycle to follow
// (a PLL relock that resolves with nothing to reapply, or with an in-place apply): drivers
// up now, audio unmuted after the same AUDIO_WAIT the cycle gives the sink to lock first.
void adv7511_tmds_resume_output(void) {
    adv7511_power_up_tmds();
    s_cycle_stage_start = HAL_GetTick();
    s_cycle_stage = TMDS_CYCLE_AUDIO_WAIT;
}

void adv7511_tmds_cycle_service(void) {
    if (s_cycle_stage == TMDS_CYCLE_IDLE) {
        return;
    }
    uint32_t elapsed = HAL_GetTick() - s_cycle_stage_start;

    switch (s_cycle_stage) {
    case TMDS_CYCLE_QUIET_WAIT:
        // Hold TMDS lines quiet so the TV's HDMI receiver PHY drops CDR lock and
        // re-acquires the new clock cleanly.
        if (elapsed >= TMDS_QUIET_MS) {
            // Reopen the input clock gate now, before the poll: the poll waits for the input
            // PLL lock (0x9E[4]), which needs a live clock -- with the gate closed it could
            // never lock and every apply burned the full ~190 ms window. Nothing reaches the
            // sink early: the TMDS outputs stay down until the poll powers them up.
            adv7511_enable_video();
            s_cycle_poll_attempts = 0;
            s_cycle_next_poll = HAL_GetTick(); // poll immediately on first service() call
            s_cycle_stage = TMDS_CYCLE_PLL_POLL;
        }
        break;

    case TMDS_CYCLE_PLL_POLL:
        // Wait for GPU PLL to stabilize before unmuting TMDS. Immediate first check,
        // then up to 19 more, 10ms apart, 20 checks total.
        if ((int32_t)(HAL_GetTick() - s_cycle_next_poll) >= 0) {
            bool locked = (adv7511_read_register(0x9E) >> 4) & 0x01;
            s_cycle_poll_attempts++;
            if ((locked || s_cycle_poll_attempts >= 20) && edid_output_hold()) {
                s_cycle_next_poll = HAL_GetTick() + 10;   // output held for the EDID
            } else if (locked || s_cycle_poll_attempts >= 20) {
                adv7511_power_up_tmds();
                // Recorded here, not at full cycle completion -- cycle_begin()'s back-to-back
                // diagnostic is measured from this point.
                last_tmds_cycle_end = HAL_GetTick();
                s_cycle_stage_start = HAL_GetTick();
                s_cycle_stage = TMDS_CYCLE_AUDIO_WAIT;
            } else {
                s_cycle_next_poll = HAL_GetTick() + 10;
            }
        }
        break;

    case TMDS_CYCLE_AUDIO_WAIT:
        // Give the sink time to lock the TMDS clock (from power-up) before audio returns.
        if (elapsed >= TMDS_AUDIO_WAIT_MS &&
            (adv7511_pixel_clock_last_hz() != 0 || elapsed >= TMDS_AUDIO_CTS_MAX_MS)) {
            adv7511_tmds_soft_clock_done();
            adv7511_unmute_audio();
            s_cycle_stage = TMDS_CYCLE_IDLE;
        }
        break;

    default:
        s_cycle_stage = TMDS_CYCLE_IDLE;
        break;
    }
}

// A mute requested over SMBus (WRITE_AUDIO_MUTE) holds through the firmware's own unmutes
// (mode changes, TMDS cycles) until released.
static bool s_audio_mute_hold = false;
void adv7511_set_audio_mute_hold(bool hold) {
    s_audio_mute_hold = hold;
    if (hold) adv7511_mute_audio(); else adv7511_unmute_audio();
}
bool adv7511_audio_mute_held(void) {
    return s_audio_mute_hold;
}

void adv7511_unmute_audio(void) {
    if (s_audio_mute_hold) return;
    // S/PDIF input on (normally already on -- covers a path that left it off; idempotent).
    adv7511_update_register(0x0B, 0b10000000, 0b10000000);
    // Audio Sample Packets back on (see adv7511_mute_audio()).
    adv7511_update_register(0x44, 0b00100000, 0b00100000);
}

static adv_color_space current_color_space = 0;

void adv7511_set_color_space(adv_color_space cs) {
    current_color_space = cs;
}

adv_color_space adv7511_get_color_space(void) {
    return current_color_space;
}

// The applied mode's internal VIC -- the one VIC-dependent input of the Auto colour rule. Set
// at the top of adv7511_apply_video_timing(), before its CSC decision, so that decision and
// the AVI InfoFrame both see the mode being applied, not the previous one. A static because
// the colour-space readers (CLI, SMBus) have no VIC of their own.
static uint8_t s_active_vic = VIC_00_VIC_Unavailable;

// Auto's SD rule: every SD VIC (1-3, 17/18/21/22) is BT.601 YCbCr from the encoder, sent as
// RGB Limited through the CSC -- on HDMI sinks only, since only the AVI Q bit can say
// "limited". A DVI sink gets no InfoFrame and takes RGB as full range (16-235 would show gray
// blacks), so it falls through to RGB Full. VIC 0 means nothing applied yet.
static bool active_vic_is_sd(void) {
    return s_active_vic != VIC_00_VIC_Unavailable && !vic_is_hd(s_active_vic);
}
static bool auto_sd_rgb_limited(void) {
    return active_vic_is_sd() && edid_is_hdmi_sink();
}

adv_color_space adv7511_get_effective_color_space(void) {
    // COLOR_SPACE_AUTO_SD_RGB_FULL is a policy, not a wire format -- it must always resolve
    // here (falling out of the bottom as 5 would land in apply_video_timing()'s RGB-Full
    // else branch for HD too).
    if (current_color_space == COLOR_SPACE_AUTO || current_color_space == COLOR_SPACE_AUTO_SD_RGB_FULL) {
        // If the encoder is being fed RGB (a GPU/dashboard driving RGB), pass it through as
        // RGB -- labelling it YCbCr would wreck the colours. Unknown or YCbCr: the rules below.
        bool in_rgb = false;
        if (xbox_encoder_input_is_rgb && xbox_encoder_input_is_rgb(&in_rgb, 0) && in_rgb) {
            return COLOR_SPACE_RGB_PASSTHROUGH;
        }
        // SD on HDMI -> RGB Limited. CEA-861-D defines Q only for RGB: a sink assumes studio
        // range for YCbCr whatever Q says, and many assume PC full range for VIC 1 (VESA
        // heritage) and the PAL 50 Hz 480-line shapes, showing reference black (16) as ~6.3%
        // gray. Through the CSC into RGB, sinks do obey Q. Limited, not Full: a 1:1 code
        // mapping, where Full would stretch 220 codes over 256 and band. Confirmed live on
        // VIC 1-3; the 576-line VICs follow for consistency (not yet confirmed live).
        if (current_color_space == COLOR_SPACE_AUTO_SD_RGB_FULL) {
            // The user's override: every SD mode, HDMI or DVI, gets RGB Full (16-235 expanded
            // to 0-255, Q = Full), so black is code 0 for sinks that ignore Q = Limited --
            // trading the banding for guaranteed black. HD falls through as in Auto.
            if (active_vic_is_sd()) {
                return COLOR_SPACE_RGB_FULL;
            }
        } else if (auto_sd_rgb_limited()) {
            return COLOR_SPACE_RGB_LIMITED;
        }
        if (edid_supports_yuv444()) {
            return COLOR_SPACE_PASSTHROUGH;
        } else {
            return COLOR_SPACE_RGB_FULL;
        }
    }
    return current_color_space;
}

// Auto names are built from the resolver's own result (index = effective space - 1), so the
// name can't drift from what is applied.
static const char * const k_auto_cs_names[2][4] = {
    { "Auto (RGB Full)", "Auto (RGB Limited)", "Auto (YCbCr 444)", "Auto (RGB-In)" },
    { "Auto+SD Full (RGB Full)", "Auto+SD Full (RGB Limited)", "Auto+SD Full (YCbCr 444)",
      "Auto+SD Full (RGB-In)" },
};

const char* adv7511_get_color_space_name(void) {
    if (current_color_space == COLOR_SPACE_AUTO || current_color_space == COLOR_SPACE_AUTO_SD_RGB_FULL) {
        uint8_t eff = (uint8_t)adv7511_get_effective_color_space();   // 1..4 by construction
        if (eff < COLOR_SPACE_RGB_FULL || eff > COLOR_SPACE_RGB_PASSTHROUGH) {
            return "Unknown";   // never index the table with a policy/garbage value
        }
        return k_auto_cs_names[current_color_space == COLOR_SPACE_AUTO_SD_RGB_FULL][eff - 1];
    }
    switch (current_color_space) {
        case COLOR_SPACE_RGB_FULL:
            return "RGB Full";
        case COLOR_SPACE_RGB_LIMITED:
            return "RGB Limited";
        case COLOR_SPACE_PASSTHROUGH:
            return "Passthrough";
        case COLOR_SPACE_RGB_PASSTHROUGH:
            return "RGB Passthrough";
        default:
            return "Unknown";
    }
}

void update_avi_infoframe(const bool widescreen, const uint8_t vic) {
    bool is_hdmi = edid_is_hdmi_sink();

    // [1] HDMI (1) or DVI (0) Mode Select
    adv7511_update_register(0xAF, 0b00000010, is_hdmi ? 0b00000010 : 0b00000000);

    // If connected to a legacy DVI monitor, do not send HDMI InfoFrames or Audio packets
    if (!is_hdmi) {
        return;
    }

    // Determine if HD content for colorimetry selection
    const bool is_hd = vic_is_hd(vic);

    // [6] Start AVI Infoframe Update
    adv7511_update_register(0x4A, 0b01000000, 0b01000000);

    adv_color_space eff_cs = adv7511_get_effective_color_space();

    // AVI data bytes DB1-DB5 are registers 0x55-0x59, written as one 5-byte burst.

    // DB1 (0x55): [6:5] output format RGB(00)/YCbCr444(10); [4] active-format-info present;
    // [1:0] scan info: Underscan (10) if supported/PC, Overscan (01), else Default (00).
    uint8_t out_fmt = (eff_cs == COLOR_SPACE_PASSTHROUGH) ? 0b01000000 : 0b00000000;
    uint8_t scan_mode = user_config_get()->scan_mode;
    uint8_t scan_info = (scan_mode == SCAN_PC)   ? 0b00000010 :   // Underscan / 1:1
                        (scan_mode == SCAN_TV)   ? 0b00000001 :   // Overscan
                        edid_supports_underscan() ? 0b00000010 : 0b00000000;

    // DB2 (0x56): [7:6] colorimetry; [5:4] picture aspect 16:9 (10) / 4:3 (01); [3:0] active
    // format = coded frame (1000). VICs 3/18/22 are 16:9 by definition -- 4:3 next to them is
    // a protocol violation some sinks reject -- so this holds even for a direct call.
    const bool aspect_16_9 = widescreen || vic_is_16_9(vic);

    // DB3 (0x57): [7] ITC = 1 (IT content: game mode); [3:2] Q, always explicit:
    //  - Full (10) for RGB Full and RGB passthrough -- the GPU's framebuffer output is full
    //    range, and Default would mean Limited on most VICs (crushing 0-15, clipping 236-255);
    //  - Limited (01) for RGB Limited and YCbCr passthrough -- studio-range data; Default
    //    would mean Full on VIC 1 and show gray blacks (seen on PAL SD modes).
    uint8_t q_bits = (eff_cs == COLOR_SPACE_RGB_FULL || eff_cs == COLOR_SPACE_RGB_PASSTHROUGH)
                     ? 0b00001000 : 0b00000100;

    // DB4 (0x58): the VIC. Honest mode writes the wire VIC (s_wire_vic). In auto mode the chip
    // sends its own detected VIC (read back at 0x3D) and ignores this byte, so it just holds
    // the raster VIC for register dumps.

    // DB5 (0x59): [7:6] YQ = 00, [5:4] CN = 11 (game content, with ITC). [3:0] pixel repetition
    // left 0 (x1, which every mode uses).

    // Colorimetry (DB2 [7:6]): No Data (00) on VIC 1 -- 640x480 is IT/VESA content with no
    // YCbCr matrix to describe, and BT.601 there makes some TVs apply broadcast-video
    // processing -- BT.709 (10) on HD, BT.601 (01) on other SD.
    const uint8_t colorimetry_bits = (vic == VIC_01_VGA_640x480_4_3) ? 0b00000000
                                    : is_hd ? 0b10000000 : 0b01000000;

    uint8_t avi_db[5] = {
        out_fmt | 0b00010000 | scan_info,
        (uint8_t)(colorimetry_bits | (aspect_16_9 ? 0b00101000 : 0b00011000)),
        (uint8_t)(0b10000000 | q_bits),
        wire_vic_honest() ? s_wire_vic : vic,
        0b00110000,
    };
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_MAIN_I2C_ADDR, 0x55,
                      I2C_MEMADD_SIZE_8BIT, avi_db, 5, 20);

    // [6] End AVI Infoframe Update
    adv7511_update_register(0x4A, 0b01000000, 0b00000000);
}

void adv7511_set_de_geometry(uint16_t hs_delay, uint16_t vs_delay, uint16_t h_active, uint16_t v_active, uint8_t interlaced_offset) {
    const uint16_t adv_delay_hs = (hs_delay > 0) ? (hs_delay - 1) : 0;
    // Single 6-byte burst write for 0x35-0x3A instead of 6 separate I2C transactions.
    uint8_t geom_buf[6] = {
        (uint8_t)(adv_delay_hs >> 2),
        (uint8_t)((0b00111111 & (uint8_t)vs_delay) | (0b11000000 & (uint8_t)(adv_delay_hs << 6))),
        (uint8_t)((interlaced_offset << 5) | (h_active >> 7)),
        (uint8_t)(h_active << 1),
        (uint8_t)(v_active >> 4),
        (uint8_t)(v_active << 4),
    };
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_MAIN_I2C_ADDR, 0x35, I2C_MEMADD_SIZE_8BIT, geom_buf, 6, 20);

    adv7511_write_register(0xDC, (uint8_t)((interlaced_offset & 0x07) << 5));   // 0 when progressive
}

// Sync adjustment: regenerated HSYNC/VSYNC placement and duration (0xD7-0xDB), enabled by
// 0x41[1].
void adv7511_set_sync_adjust(bool enable, uint16_t hs_placement, uint16_t hs_duration, uint16_t vs_placement, uint16_t vs_duration) {
    if (enable) {
        // Single 5-byte burst write for 0xD7-0xDB instead of 5 separate I2C transactions.
        uint8_t sync_buf[5] = {
            (uint8_t)(hs_placement >> 2),
            (uint8_t)((uint8_t)(hs_placement << 6) | (uint8_t)(hs_duration >> 4)),
            (uint8_t)((uint8_t)(hs_duration << 4) | (uint8_t)(vs_placement >> 6)),
            (uint8_t)((uint8_t)(vs_placement << 2) | (uint8_t)(vs_duration >> 8)),
            (uint8_t)(vs_duration),
        };
        HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_MAIN_I2C_ADDR, 0xD7, I2C_MEMADD_SIZE_8BIT, sync_buf, 5, 20);
        adv7511_update_register(0x41, 0b00000010, 0b00000010);
    } else {
        adv7511_update_register(0x41, 0b00000010, 0b00000000);
    }
}

// Which path last programmed the picture, BIOS or standalone. Set in the apply itself (the
// one place timing reaches the chip), so it always describes what is on screen.
static bool g_active_mode_is_bios = false;

bool adv7511_active_mode_is_bios(void) {
    return g_active_mode_is_bios;
}

// The one exception to "set only by the apply": BIOS taking over a picture standalone already
// applied identically (adv7511_timing_would_change() is false). Only ownership changes; the
// screen doesn't.
void adv7511_set_active_mode_is_bios(bool is_bios) {
    g_active_mode_is_bios = is_bios;
}

// False until the first real mode is programmed this boot. The supervisor keeps the TMDS
// outputs down through the cold-boot PLL settle until then -- otherwise the init placeholder
// (VIC 2 AVI, default DE geometry) would go out against the real clock. Reset with the
// applied timing on every chip reinit.
static bool g_any_mode_applied = false;

bool adv7511_any_mode_applied(void) {
    return g_any_mode_applied;
}

// What the last apply actually programmed (not what was requested), so
// adv7511_timing_would_change() can skip a TMDS cycle -- a blackout -- for identical state.
// Common: distinct mode_index values resolve to the same timing (Focus 0x07/0x08/0x11).
static XboxVideoTiming s_applied_timing;
static bool s_applied_ws = false;
static bool s_applied_valid = false;
// The colour space the last apply programmed (output format 0x16[0] + CSC): what is on the
// wire, unlike adv7511_get_effective_color_space(), which re-resolves from live inputs.
static adv_color_space s_applied_cs = COLOR_SPACE_AUTO;

// The chip was just reset: nothing it held before counts as applied (see the forward decl).
static void invalidate_applied_timing(void) {
    s_applied_valid = false;
    g_any_mode_applied = false;
}

// The AVI aspect flag that actually reaches the sink: the caller's widescreen request OR
// the VIC's own inherent 16:9-ness. Shared by the apply path and
// adv7511_timing_would_change() so the two can't disagree about what "same aspect" means.
bool timing_ws_infoframe(const XboxVideoTiming * const timing, const bool widescreen) {
    return widescreen || vic_is_16_9(timing->vic);
}

// ---- Wire-VIC policy (WIRE_VIC_HONEST) ----
// The VIC declared to the sink must depend only on the signal, so every TV gets the same
// information for the same mode. The chip's own detection (auto pixel-repeat mode) doesn't:
// its line detector carries the previous raster's reading, so one mode went out as VIC 0,
// 1, 2 or 17 depending on what ran before. Per the Programming Guide, non-CEA-861 timings
// must be sent as VIC 0; a CEA VIC is declared only when geometry, scan AND pixel clock all
// match that format. Every SD raster on the Xbox pixel bus is progressive, so no 480i/576i
// VIC (1440-wide, pixel-repeated by definition) is ever declared; the 640-wide rasters
// (24.4-27.6MHz) never match VGA's 25.175MHz VIC 1; PAL 720x480@50 is not a CEA format.
// Both checks are needed: 640-wide clocks at 26.874/27.125MHz sit within 0.5% of 27MHz.

// +/-0.25% of nominal -- covers each format's 59.94/60Hz pair (0.1% apart).
static bool pclk_near(uint32_t pclk_hz, uint32_t nominal_hz) {
    uint32_t tol = nominal_hz / 400;
    return pclk_hz + tol >= nominal_hz && pclk_hz <= nominal_hz + tol;
}

static uint8_t honest_wire_vic(const XboxVideoTiming * const t, const bool ws_infoframe,
                               const uint32_t pclk_hz) {
    // HD geometry is only ever the Xbox's standard 720p/1080i timing, so it's declared before
    // the clock settles too (no 0 -> 4/5 step ~0.6s into every HD switch, and the
    // bootloader, which has no settled clock, still declares HD correctly). A settled clock
    // outside the CEA windows still forces 0.
    const bool hd_clk = pclk_hz == 0 ||
                        pclk_near(pclk_hz, 74250000UL) || pclk_near(pclk_hz, 74175824UL);
    if (t->active_w == 1280 && t->active_h == 720 && !t->interlaced_offset) {
        return hd_clk ? VIC_04_720p_60_16_9 : VIC_00_VIC_Unavailable;
    }
    if (t->active_w == 1920 && t->active_h == 540 && t->interlaced_offset) {
        return hd_clk ? VIC_05_1080i_60_16_9 : VIC_00_VIC_Unavailable;
    }
    if (pclk_hz == 0) {
        return VIC_00_VIC_Unavailable;   // SD: no settled clock yet -- "unknown" is never wrong
    }
    if (t->interlaced_offset) {
        return VIC_00_VIC_Unavailable;
    }
    if (t->active_w == 720 && t->active_h == 480 &&
        (pclk_near(pclk_hz, 27000000UL) || pclk_near(pclk_hz, 27027000UL))) {
        return ws_infoframe ? VIC_03_480p_60_16_9 : VIC_02_480p_60__4_3;
    }
    if (t->active_w == 720 && t->active_h == 576 && pclk_near(pclk_hz, 27000000UL)) {
        return ws_infoframe ? VIC_18_576p_50_16_9 : VIC_17_576p_50__4_3;
    }
    return VIC_00_VIC_Unavailable;
}

// When the wire-VIC service started holding a CEA VIC through a raster-sized clock move
// (0 = not holding) -- see adv7511_wire_vic_service().
static uint32_t s_wire_hold_since = 0;
#define WIRE_VIC_RASTER_HOLD_MS 2000UL

// The VIC to program into 0x3C for an apply: the policy VIC in honest mode, the internal
// VIC in auto mode (0x3C is then ignored by the chip and only read back by
// adv7511_read_active_vic()). pclk_hz is the clock the raster being applied is known to run
// at, or 0 if unknown -- a raster change passes 0, because the clock settled at that moment
// belongs to the previous raster; adv7511_wire_vic_service() decides once the new one settles.
static uint8_t wire_vic_for(const XboxVideoTiming * const t, const bool ws_infoframe,
                            const uint32_t pclk_hz) {
    s_wire_pclk = pclk_hz;
    s_wire_hold_since = 0;
    return wire_vic_honest() ? honest_wire_vic(t, ws_infoframe, pclk_hz) : t->vic;
}

// Honest mode, every main-loop pass: re-judge the wire VIC from the settled clock and update
// it in place (0x3C + AVI) when it changes.
void adv7511_wire_vic_service(void) {
    if (!wire_vic_honest() || !s_applied_valid || adv7511_tmds_cycle_busy()) {
        return;
    }
    // No settled clock (a mode-set disruption, an encoder reset under an unchanged mode) is
    // not evidence of a non-CEA clock -- hold the current wire VIC until a clock settles.
    const uint32_t pclk = adv7511_pixel_clock_settled_hz();
    if (pclk == 0) {
        return;
    }
    uint8_t w = honest_wire_vic(&s_applied_timing, s_applied_ws, pclk);
    if (w == s_wire_vic) {
        s_wire_pclk = pclk;
        s_wire_hold_since = 0;
        return;
    }
    // Stepping up to the raster's own CEA VIC happens on the reported clock. Moving OFF a
    // CEA VIC waits for the classifier window, and -- when the clock moved more than 5% from
    // the one this VIC was judged on -- for the mode change that move means: no raster's
    // clock varies that much (the widest same-raster spread is 720x480 at 27.000 vs 27.563,
    // 2%), and the arbiter's apply re-judges the wire VIC itself, sometimes ~300ms after the
    // clock is stable (its blind/width levels confirm first). Without this the old mode got a
    // brief in-place VIC/AVI change just before its replacement. Bounded: if no apply comes
    // within WIRE_VIC_RASTER_HOLD_MS, the raster really runs on this clock -- go honest.
    if (s_wire_vic != VIC_00_VIC_Unavailable) {
        if (!adv7511_pixel_clock_stable()) {
            return;
        }
        // HD is declared before its clock settles, so s_wire_pclk can still be 0 here.
        const uint32_t ref = s_wire_pclk ? s_wire_pclk : 74250000UL;
        const uint32_t moved = (pclk > ref) ? pclk - ref : ref - pclk;
        if (moved > ref / 20) {
            const uint32_t now = HAL_GetTick();
            if (s_wire_hold_since == 0) {
                s_wire_hold_since = now ? now : 1u;   // 0 means "not holding"
            }
            if (now - s_wire_hold_since < WIRE_VIC_RASTER_HOLD_MS) {
                return;
            }
        }
    }
    s_wire_hold_since = 0;
    s_wire_pclk = pclk;
    s_wire_vic = w;
    adv7511_write_register(0x3C, w);
    update_avi_infoframe(s_applied_ws, s_applied_timing.vic);
    debug_log("[ADV] wire VIC %u\r\n", w);
}

// Reapply the current mode after the sink changed (EDID retry, hotplug).
void adv7511_refresh_sink_config(void) {
    if (s_applied_valid) {
        adv7511_apply_video_timing(&s_applied_timing, s_applied_ws, g_active_mode_is_bios);
    }
}

bool adv7511_output_held_for_edid(void) {
    return s_cycle_stage == TMDS_CYCLE_PLL_POLL && adv7511_tmds_powered_down();
}

bool adv7511_applied_mode(XboxVideoTiming *timing, bool *widescreen) {
    *timing = s_applied_timing;
    *widescreen = s_applied_ws;
    return s_applied_valid;
}

adv_color_space adv7511_applied_color_space(void) {
    return s_applied_cs;
}

uint8_t adv7511_wire_vic(void) {
    // Honest mode: manual pixel repeat makes the chip transmit 0x3C, which is s_wire_vic.
    // Auto mode: the chip picks the transmitted VIC itself; 0x3D is its readback.
    return wire_vic_honest() ? s_wire_vic : adv7511_read_register(0x3D);
}

// Exact wording matters: xhd_daemon.py and xhd_gui/server.py both key off "Actual VIC Sent :"
// (auto-capture trigger / mode-transition tracking), so this string is a host-side contract.
// It reports the one VIC on the wire (adv7511_wire_vic()), never the internal VIC; a later
// honest-mode change from the settled clock is logged by adv7511_wire_vic_service().
void adv7511_log_vic_sent(void) {
    const uint8_t v = adv7511_wire_vic();
    debug_log("Actual VIC Sent : VIC %u (0x%02X)\r\n", v, v);
}

uint8_t adv7511_read_active_vic(void) {
    if (!wire_vic_honest()) {
        return adv7511_read_register(0x3C);   // auto mode: 0x3C still holds the internal VIC
    }
    // A chip reset (HPD low resets the register map) clears 0x3B to 0 -- reported as VIC_00
    // exactly as 0x3C's own reset value used to be, so callers still see "nothing applied".
    if (adv7511_read_register(0x3B) != ADV_PR_MANUAL_X1 || !s_applied_valid) {
        return VIC_00_VIC_Unavailable;
    }
    return s_applied_timing.vic;
}

// Deferred-cycle fast path (see adv7511_tmds_cycle_begin_deferred()). Same raster as the one
// on the chip -- everything in XboxVideoTiming equal but the VIC -- means an aspect-only
// change: 640 anamorphic <-> 4:3 (VIC 1 both ways) or a 4:3/16:9 VIC pair (2/3, 17/18, 21/22).
// That needs only 0x17[1], the wire VIC (0x3C) and the AVI InfoFrame, which goes out every
// frame -- so the sink sees it with no TMDS drop and no audio mute. The CSC choice depends
// on SD vs HD, which the same raster can't change. False (nothing written) when the raster
// differs or nothing has been applied yet.
static bool apply_raster_unchanged(const XboxVideoTiming * const timing, const bool widescreen,
                                   const bool from_bios) {
    XboxVideoTiming raster = *timing;
    raster.vic = s_applied_timing.vic;
    if (!s_applied_valid || memcmp(&raster, &s_applied_timing, sizeof(raster)) != 0) {
        return false;
    }
    const bool ws = timing_ws_infoframe(timing, widescreen);
    s_active_vic = timing->vic;
    g_active_mode_is_bios = from_bios;
    if (smbus_telemetry_note_mode_apply) smbus_telemetry_note_mode_apply();
    adv7511_update_register(0x17, 0b00000010, ws ? 0b00000010 : 0b00000000);
    // Same raster, so the clock it was last judged on still describes it.
    const uint32_t settled = adv7511_pixel_clock_settled_hz();
    s_wire_vic = wire_vic_for(timing, ws, settled ? settled : s_wire_pclk);
    adv7511_write_register(0x3C, s_wire_vic);
    update_avi_infoframe(ws, timing->vic);
    s_applied_timing.vic = timing->vic;
    s_applied_ws = ws;
    s_cycle_applied_in_place = true;
    debug_log("[ADV] in-place, raster unchanged\r\n");
    adv7511_log_vic_sent();
    return true;
}

// Programs a resolved timing -- the one register sequence both the BIOS and standalone paths
// use.
void adv7511_apply_video_timing(const XboxVideoTiming * const timing, const bool widescreen, const bool from_bios) {
    // Deferred bracket (see adv7511_tmds_cycle_begin_deferred()): an unchanged raster is
    // applied in place, anything else closes the gate now, before any register below.
    if (s_cycle_deferred) {
        s_cycle_deferred = false;
        if (apply_raster_unchanged(timing, widescreen, from_bios)) {
            return;
        }
        adv7511_tmds_cycle_begin();
    }

    // First, so the CSC decision below and the AVI InfoFrame both see this mode's VIC.
    s_active_vic = timing->vic;

    // An HPD blip since the last apply resets 0x41[6] to Main Power Down (POR default --
    // see adv7511_ensure_powered_up()'s own comment); every register write below would be
    // a silent no-op until this runs. Single choke point for every mode-apply path.
    adv7511_ensure_powered_up();

    g_active_mode_is_bios = from_bios;
    g_any_mode_applied = true;
    if (smbus_telemetry_note_mode_apply) smbus_telemetry_note_mode_apply();

    // Auto Audio Pop/Click Protection: Mute audio while re-programming timing raster
    adv7511_mute_audio();

    const bool is_hd = vic_is_hd(timing->vic);

    // Apply dynamic color space conversion (Auto EDID, Full Range, Limited Range, or Passthrough)
    adv_color_space cs = adv7511_get_effective_color_space();
    s_applied_cs = cs;
    if (cs == COLOR_SPACE_RGB_PASSTHROUGH) {
        // Input is already RGB (a GPU/dashboard driving RGB directly) -- no CSC, output RGB. The AVI
        // InfoFrame's DB1 [6:5] follows from eff_cs in update_avi_infoframe().
        adv7511_disable_csc();
        adv7511_update_register(0x16, 0b00000001, 0b00000000); // Output is RGB
    } else if (cs == COLOR_SPACE_PASSTHROUGH) {
        adv7511_disable_csc();
        adv7511_update_register(0x16, 0b00000001, 0b00000001); // Output is YCbCr
    } else if (cs == COLOR_SPACE_RGB_LIMITED) {
        adv7511_update_register(0x16, 0b00000001, 0b00000000); // Output is RGB
        adv7511_apply_csc(is_hd ? (uint8_t *)CscYuvToRgb709Limited : (uint8_t *)CscYuvToRgb601Limited);
    } else { // COLOR_SPACE_RGB_FULL
        adv7511_update_register(0x16, 0b00000001, 0b00000000); // Output is RGB
        adv7511_apply_csc(is_hd ? (uint8_t *)CscYuvToRgb709 : (uint8_t *)CscYuvToRgb601);
    }

    // Force DE Generator re-synchronization to incoming VSync phase across mode switches / soft resets
    adv7511_update_register(0x17, 0b00000001, 0b00000000);

    // The AVI aspect actually sent: the request, or 16:9 for VICs that are 16:9 by definition
    // (HD, 3, 18, 22), so BIOS and standalone can't disagree. Also feeds 0x17[1] below.
    const bool ws_infoframe = timing_ws_infoframe(timing, widescreen);

    // 0x17: [6:5] sync polarity -- positive (00) for HD, negative (11) for SD; [1] aspect,
    // which only steers the line detector between same-timing VIC pairs (2/3, 17/18, 21/22) so
    // 0x3E reports the right one; [0] DE generator on.
    adv7511_update_register(0x17, 0b01100011,
                            (is_hd ? 0b00000000 : 0b01100000) |
                            (ws_infoframe ? 0b00000010 : 0b00000000) |
                            0b00000001);

    // Set DE active video raster geometry
    adv7511_set_de_geometry(timing->delay_hs, timing->delay_vs, timing->active_w, timing->active_h, timing->interlaced_offset);

    // Set Sync Adjustment
    adv7511_set_sync_adjust(timing->sync_adjust_enabled, timing->hsync_placement, timing->hsync_duration, timing->vsync_placement, timing->vsync_duration);

    // 0xD0: [3:2] no sync pulse; [1] DE generation first, then sync adjustment.
    adv7511_update_register(0xD0, 0b00001110, 0b00001110);

    // 0x3C ("VIC Manual"): the wire VIC (see the wire-VIC policy above). In honest mode
    // (manual pixel repeat) the chip transmits exactly this; in auto mode it's ignored by
    // the chip and only holds the internal VIC for adv7511_read_active_vic().
    s_wire_vic = wire_vic_for(timing, ws_infoframe, 0);
    adv7511_write_register(0x3C, s_wire_vic);

    update_avi_infoframe(ws_infoframe, timing->vic);

    s_applied_timing = *timing;
    s_applied_ws = ws_infoframe;
    s_applied_valid = true;

    adv7511_log_vic_sent();
}

bool adv7511_timing_would_change(const XboxVideoTiming * const timing, const bool widescreen) {
    // Compares the aspect actually sent (ws_infoframe), not the raw argument: a 4:3 request on
    // a 16:9-only VIC programs identically. memcmp is safe because xbox_get_video_timing()
    // zeroes the whole struct, padding included.
    return !s_applied_valid ||
           timing_ws_infoframe(timing, widescreen) != s_applied_ws ||
           memcmp(timing, &s_applied_timing, sizeof(*timing)) != 0;
}
