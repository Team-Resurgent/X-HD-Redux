// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT

#include <string.h>
#include "adv7511.h"
#include "adv7511_xbox.h"
#include "../shared/adv7511_i2c.h"
#include "debug.h"
#include "adv7511_edid.h"
#include "user_config.h"

uint8_t adv7511_read_register(const uint8_t address) {
    uint8_t data = 0;
    i2c1_read(ADV7511_MAIN_I2C_ADDR, address, &data, 1, 20);
    return data;
}

void adv7511_write_register(const uint8_t address, uint8_t value) {
    i2c1_write(ADV7511_MAIN_I2C_ADDR, address, &value, 1, 20);
}

void adv7511_update_register(const uint8_t address, const uint8_t mask, uint8_t new_value) {
    const uint8_t current = adv7511_read_register(address);
    uint8_t updated = (current & ~mask) | (new_value & mask);
    adv7511_write_register(address, updated);
}

void adv7511_struct_init(adv7511 *encoder) {
    encoder->interrupt = 0;
    encoder->vic = 0;
}

void adv7511_power_up(void) {
    // Force a genuine Power-Down -> Power-Up transition on 0x41[6]: writing 0 alone
    // isn't guaranteed to produce an edge if the chip's own register state already
    // reads 0 (an MCU-only reset doesn't reset the ADV7511 chip itself).
    adv7511_update_register(0x41, 0b01000000, 0b01000000); // force Power Down = 1
    HAL_Delay(10);

    // Power up: the Programming Guide's power-up step is 0x41[6] = 0 (with HPD high).
    // Same final value (0x10) this always ended on; the former 0x10 -> 0x00 -> 0x10
    // toggle and its 110ms of waits had no documented purpose.
    adv7511_write_register(0x41, 0x10);
    HAL_Delay(10);
}

// Table 74 (ADV7511W Programming Guide, Sec 4.7.1): HPD Pin = 0 resets the whole
// 0x00-0x93 main register map to its power-on defaults, and 0x41[6]'s POR default is 1
// (Main Power Down) -- so any HPD glitch re-arms Main Power Down, and nothing after
// adv7511_power_up() clears it again. A stuck Main Power Down leaves 0x9E at 0x00 and
// gate-cycling alone can't recover it. Checked on every relock/reconnect/mode-apply --
// a plain register read when the bit is already clear.
// Main Power Down also clears the Packet Memory map (ADV7511 Programming Guide Rev G, Table
// 97), so the SPD InfoFrame (and the ALLM packet) goes out again.
void adv7511_ensure_powered_up(void) {
    if (adv7511_read_register(0x41) & 0x40) {
        adv7511_update_register(0x41, 0b01000000, 0b00000000);
        adv7511_init_spd_infoframe();
    }
}

void adv_handle_interrupts(adv7511 *encoder) {
    // Clears the ADV7511's interrupt flags (INT0 and INT1) from the main loop.
    if (encoder->interrupt) {
        encoder->interrupt = 0;
        uint8_t interrupt_register = adv7511_read_register(0x96);
        adv7511_write_register(0x96, interrupt_register); // Clear and re-enable interrupts (INT0)
        // 0x96[7] HPD interrupt: any HPD transition, latched -- including a blip too short
        // for the supervisor's per-pass 0x42 poll to see. HPD=0 POR-resets 0x41[6] to Main
        // Power Down (see adv7511_ensure_powered_up()), and with the PLL then unable to
        // lock, no relock/mode-apply path would ever run to power it back up. 0x94/0x96 sit
        // above the 0x00-0x93 range HPD resets, so this flag survives the blip.
        if (interrupt_register & 0x80) {
            adv7511_ensure_powered_up();
        }
        // Also clear INT1 (0x97) -- the chip's INT pin is a shared OR of both registers'
        // pending bits, so an uncleared INT1 bit would latch the (edge-triggered) line
        // low and stop this handler from ever firing again.
        uint8_t interrupt_register_1 = adv7511_read_register(0x97);
        adv7511_write_register(0x97, interrupt_register_1);
    }
}

void adv7511_disable_csc() {
    adv7511_update_register(0x18, 0b10000000, 0b00000000);
}

void adv7511_apply_csc(const uint8_t * const coefficients) {
    // CSC coefficients live in contiguous registers 0x18-0x2F (24 bytes; writing 0x18[7]=1
    // in coefficients[0] enables CSC). One auto-incrementing burst instead of 24 separate
    // single-byte I2C transactions. Buffered: 0x1A[5] (Coefficient Update, shared with A2's
    // high bits) goes on before the burst and stays on in it, so the chip keeps the old matrix
    // until adv7511_csc_release() puts the whole new one out at the next Vsync (programming
    // guide p.57). Written directly, each coefficient
    // took effect as it landed, mid-frame.
    uint8_t buf[24];
    memcpy(buf, coefficients, sizeof buf);
    buf[2] |= 0x20;
    adv7511_update_register(0x1A, 0x20, 0x20);
    i2c1_write(ADV7511_MAIN_I2C_ADDR, 0x18, buf, sizeof buf, 20);
}

void adv7511_csc_release(void) {
    adv7511_update_register(0x1A, 0x20, 0x00);
}

// 0xD6[0] is the ADV7511's "Video Input and Clock Gating" bit -- an input-side control
// that gates the incoming pixel clock/data path into the chip (not output blanking).
// Cycling it also forces the chip's internal clock-recovery/PLL to lose and re-acquire
// reference, used here to clear a stuck PLL lock.
// Always mutes audio sample packets first: audio rides the pixel clock (ACR),
// so packets must never be enabled while the input clock is gated. Re-enabling video does
// NOT unmute -- that stays with each caller's own settle discipline (tmds cycle's
// AUDIO_WAIT, HPD reconnect, reapply_active_mode()).
void adv7511_disable_video() {
    adv7511_mute_audio();
    // [0] Gate input clock
    adv7511_update_register(0xD6, 0b00000001, 0b00000001);
}

void adv7511_enable_video() {
    // [0] Un-gate input clock
    adv7511_update_register(0xD6, 0b00000001, 0b00000000);
}

void adv7511_power_down_tmds() {
    // [5:3] Channel 0-2 power down, [2] Clock Driver power down -- the clock pair goes down
    // with the data, so the sink sees the link drop rather than a clock carrying no data.
    adv7511_update_register(0xA1, 0b00111100, 0b00111100);
}

bool adv7511_tmds_powered_down(void) {
    return (adv7511_read_register(0xA1) & 0b00111000) != 0;
}

void adv7511_power_up_tmds() {
    // Programming guide, TMDS Power-Down: enable Soft TMDS Clock Turn On (0xD6[4]) before
    // powering the clock driver up, and disable it once the clock is active
    // (adv7511_tmds_soft_clock_done(), called by the cycle's audio stage).
    adv7511_update_register(0xD6, 0b00010000, 0b00010000);
    // [5:3] Channel 0-2 power up, [2] Clock Driver power up
    adv7511_update_register(0xA1, 0b00111100, 0b00000000);
}

void adv7511_tmds_soft_clock_done(void) {
    adv7511_update_register(0xD6, 0b00010000, 0b00000000);
}

// Live incoming pixel clock, derived from the HDMI audio-clock-regeneration N/CTS
// registers (f_pixel = CTS * 128 * f_s / N, HDMI spec 7.2). CTS is auto-generated by the
// chip from the real incoming TMDS clock and is hardware read-only in auto mode. 0x04 is
// dual-purpose (SPDIF Sampling Frequency status in [7:4], CTS[19:16] in [3:0]) -- must
// mask to the low nibble or the SPDIF-Fs bits corrupt the result.
//
// N and f_s are both fixed: init_adv_audio() programs N=6144 and
// adv7511_set_video_input_format() programs 0x15[7:4]=48kHz on every chip (re)init, and
// nothing else writes either. The Xbox APU only ever emits 48kHz SPDIF, so
// 128 * 48000 / 6144 = 1000 exactly and CTS is the pixel clock in kHz. 0x04[7:4] is
// deliberately not consulted: on this hardware anything other than the 48kHz code there
// is a transient SPDIF-lock reading, not a real rate.
uint32_t adv7511_get_pixel_clock_hz(void) {
    // CTS[19:0] (0x04[3:0]:0x05:0x06) is a live hardware counter the chip's own ACR
    // logic updates on its own schedule. Read as a single atomic 3-byte burst rather
    // than 3 separate single-byte reads, which could otherwise tear across a real
    // chip-side update mid-sequence.
    uint8_t cts_buf[3] = {0};
    i2c1_read(ADV7511_MAIN_I2C_ADDR, 0x04, cts_buf, 3, 20);
    uint32_t cts = (((uint32_t)cts_buf[0] & 0x0F) << 16) |
                   ((uint32_t)cts_buf[1] << 8) |
                   (uint32_t)cts_buf[2];

    // No CTS -> the ACR isn't running (no incoming clock / audio) -> no measurement (0).
    // No overflow: cts is a 20-bit field (max 0xFFFFF), * 1000 stays under UINT32_MAX.
    return cts * 1000UL;
}

// --- Pixel-clock stability tracker ---
// CTS (regs 0x04-0x06, read above) is a hardware ACR counter. It does not walk toward the
// true pixel clock: it reads a clean 0 for ~500-600ms after any signal disruption (ACR
// halted), then snaps to the exact final value in a single loop iteration and holds it to
// +/-1 kHz. The bad values are that 0 window, half-clock garbage (~12 MHz) during the
// ENCODER_UNKNOWN self-latch, and lone outliers right after an Xbox-side encoder reset
// (e.g. 28.196 MHz on a steady 24.937 MHz Conexant raster, with a good picture throughout).
// adv7511_pixel_clock_note() is fed the value the supervisor reads once per pass (no
// extra I2C); "stable" means a non-zero reading has held within a tight band for the
// settle window. Classifiers gate on adv7511_pixel_clock_stable(); anything that REPORTS
// the clock uses adv7511_pixel_clock_settled_hz(), never a single raw read.
// +/-50 kHz: well above the +/-1-2 kHz reading jitter, below the smallest gap between two
// real modes' clocks (126 kHz, 26.874 vs 27.000 MHz). Any move larger than this restarts the
// settle window -- a wider band let a transitional clock count as "stable" and drive the
// standalone drift re-apply (which acts on a 200 kHz move) mid mode-set.
#define PCLK_STABLE_BAND_HZ     50000UL
// Both windows must outlast a transient clock that holds steady during a mode set: a
// since-removed diagnostic logged Conexant ~27.5MHz transients holding up to ~150ms.
#define PCLK_STABLE_WINDOW_MS   300UL
// Shorter window for the REPORTED value (and the wire VIC, which is decided on it): the
// command line and host tools show a new clock 100ms sooner than the classifiers act on it. Classifiers keep
// the full PCLK_STABLE_WINDOW_MS via adv7511_pixel_clock_stable().
#define PCLK_REPORT_WINDOW_MS   200UL
#define PCLK_STABLE_FORCE_MS    3000UL     // bounded escape: never wedge classifiers off forever

static uint32_t s_pclk_last = 0;
static uint32_t s_pclk_in_band_since = 0;
// Start of the current unsettled stretch -- the FORCE_MS escape's anchor. Set by the first
// reading after a loss or by any out-of-band jump, cleared once a reading settles. (It used
// to be "first reading since loss" and never cleared, so 3s after acquisition the escape
// held stable() true for good and every later outlier passed as stable.)
static uint32_t s_pclk_unsettled_since = 0;
// Last reading that held in band for PCLK_REPORT_WINDOW_MS. Survives lone outliers and a
// lone 0 (an I2C hiccup); cleared by a sustained 0 run or a PLL loss, so a real mode change
// reports 0 ("no measurement yet") instead of the previous mode's clock.
static uint32_t s_pclk_settled_hz = 0;
static uint8_t  s_pclk_zero_run = 0;   // consecutive 0 readings, saturating

void adv7511_pixel_clock_note(uint32_t pclk_hz) {
    uint32_t now = HAL_GetTick();
    uint32_t prev = s_pclk_last;
    s_pclk_last = pclk_hz;

    if (pclk_hz == 0) {
        // Any 0 pauses the settle window (cheap, self-heals on the next real reading).
        s_pclk_in_band_since = 0;
        if (s_pclk_zero_run < 255) {
            s_pclk_zero_run++;
        }
        // A sustained loss (2+ consecutive 0s; a genuine drop produces 5-6, a lone 0 is an
        // I2C hiccup) -- an Xbox-side mode change (720<->640) drops CTS to 0 and recovers
        // without raising a PLL-loss event, so reset_stability() is never called for it.
        // Fires once, on the 2nd 0.
        if (s_pclk_zero_run == 2) {
            s_pclk_unsettled_since = 0;
            s_pclk_settled_hz = 0;
        }
        return;
    }
    s_pclk_zero_run = 0;

    uint32_t delta = (pclk_hz > prev) ? (pclk_hz - prev) : (prev - pclk_hz);
    if (delta > PCLK_STABLE_BAND_HZ) {
        s_pclk_in_band_since = 0;   // just jumped -- restart the settle window
        if (s_pclk_unsettled_since == 0) {
            s_pclk_unsettled_since = now;
        }
    } else if (s_pclk_in_band_since == 0) {
        s_pclk_in_band_since = now;
    }

    if (s_pclk_in_band_since != 0) {
        uint32_t held = now - s_pclk_in_band_since;
        if (held >= PCLK_REPORT_WINDOW_MS) {
            s_pclk_settled_hz = pclk_hz;
        }
        if (held >= PCLK_STABLE_WINDOW_MS) {
            s_pclk_unsettled_since = 0;
        }
    }
}

bool adv7511_pixel_clock_stable(void) {
    uint32_t now = HAL_GetTick();
    if (s_pclk_in_band_since != 0 &&
        (now - s_pclk_in_band_since) >= PCLK_STABLE_WINDOW_MS) {
        return true;
    }
    // Bounded escape -- if CTS genuinely stays unsettled for FORCE_MS on some pathological
    // signal, don't hold every classifier off forever; 0x3E / the VIC-candidate debounce
    // are the CTS-independent fallback.
    return s_pclk_unsettled_since != 0 && (now - s_pclk_unsettled_since) >= PCLK_STABLE_FORCE_MS;
}

uint32_t adv7511_pixel_clock_settled_hz(void) {
    return s_pclk_settled_hz;
}

uint32_t adv7511_pixel_clock_last_hz(void) {
    return s_pclk_last;
}

// Hard reset on a genuine loss of lock. _note() already re-arms itself on a clean drop to
// 0 (the common case); this additionally covers a PLL loss where CTS keeps reading stale
// non-zero garbage, which _note() would otherwise treat as a still-valid signal.
void adv7511_pixel_clock_reset_stability(void) {
    s_pclk_last = 0;
    s_pclk_in_band_since = 0;
    s_pclk_unsettled_since = 0;
    s_pclk_settled_hz = 0;
    s_pclk_zero_run = 0;
}

#ifdef BUILD_APPLICATION
// CEC Map register access (0x78, ADV7511_CEC_I2C_ADDR) -- mirrors the Main Map helpers
// above exactly, just targeting the separate I2C sub-address the CEC block lives at.
uint8_t adv7511_cec_read_register(const uint8_t address) {
    uint8_t data = 0;
    i2c1_read(ADV7511_CEC_I2C_ADDR, address, &data, 1, 20);
    return data;
}

void adv7511_cec_write_register(const uint8_t address, uint8_t value) {
    i2c1_write(ADV7511_CEC_I2C_ADDR, address, &value, 1, 20);
}

static void adv7511_cec_update_register(const uint8_t address, const uint8_t mask, uint8_t new_value) {
    const uint8_t current = adv7511_cec_read_register(address);
    uint8_t updated = (current & ~mask) | (new_value & mask);
    adv7511_cec_write_register(address, updated);
}

// Register map: the ADV7511W Programming Guide's CEC section (Tables 78/79/85/86 -- Tx frame
// 0x00/0x01-0x0F/0x10/0x11, Rx Buffer 1 frame 0x15/0x16-0x24/0x25, Rx ready/clear
// 0x49/0x4A[2:0]). Where the guide's prose is ambiguous, the exact semantics are those of the
// Linux adv7511 CEC driver:
//   - Tx frame length (0x10) is the LITERAL total byte count (header + data), not
//     operands+2 or len-1 -- the driver writes `len` with the comment "set length (data +
//     header)".
//   - Rx Buffer 1's "Ready Clear" bit (0x4A[0]) needs a momentary set-then-clear pulse to
//     actually re-arm the buffer, not a bare write of 1 left standing -- the driver sets the
//     bit and clears it again immediately, though the register table's own prose reads like a
//     plain "1 = clear out" bit.
//
// Rx: all three buffers (0x4A[3] = 1, "non-legacy", as the Linux driver), and each frame is
// copied out and its buffer freed on the poll that finds it, again as Linux does. Leaving
// frames in the chip's one buffer until something collected them failed in practice: almost
// nothing collects the relay (it is new to the console), so after one remote key press its
// release never arrived, and the TV sent no CEC again until a reboot re-announced us. The
// guide doesn't say what the chip does with a frame when no buffer is free; NACKing it fits
// that behaviour, and a TV seeing NACKs can decide the device is gone. Frames now wait for
// the relay in s_cec_q instead; the chip's buffers stay free.
// The CEC opcodes and addresses X-HD Redux's own logic uses (CEC 1.4/2.0). The request tables
// below (k_cec_answer, k_cec_no_abort) stay numeric, each entry named in its comment.
enum {
    CEC_OP_FEATURE_ABORT = 0x00, CEC_OP_IMAGE_VIEW_ON = 0x04, CEC_OP_STANDBY = 0x36,
    CEC_OP_SET_OSD_NAME = 0x47, CEC_OP_ROUTING_CHANGE = 0x80, CEC_OP_ROUTING_INFORMATION = 0x81,
    CEC_OP_ACTIVE_SOURCE = 0x82, CEC_OP_REPORT_PHYSICAL_ADDR = 0x84,
    CEC_OP_REQUEST_ACTIVE_SOURCE = 0x85, CEC_OP_SET_STREAM_PATH = 0x86,
    CEC_OP_GIVE_POWER_STATUS = 0x8F, CEC_OP_REPORT_POWER_STATUS = 0x90, CEC_OP_INACTIVE_SOURCE = 0x9D,
    CEC_OP_REPORT_FEATURES = 0xA6, CEC_OP_ABORT = 0xFF,
};
#define CEC_LA_TV           0x0
#define CEC_LA_BROADCAST    0xF   // as a destination; as an initiator it is Unregistered
#define CEC_LA_UNREGISTERED 0xF
#define CEC_DEVICE_PLAYBACK 0x04  // <Report Physical Address>'s device type
#define CEC_ABORT_UNRECOGNIZED 0x00   // <Feature Abort> reasons
#define CEC_ABORT_REFUSED      0x04

static uint8_t s_cec_tx_status = 0;
// The relay's queue: s_cec_q_n frames, the oldest at s_cec_q_head. The I2C2 ISR only reads
// the oldest (adv7511_cec_rx_pending/read) and asks for its release; the main loop pushes,
// pops and ages. Full: the oldest is dropped -- only when nothing is collecting (4 frames
// inside the 1 s hold), so no host is part-way through reading it.
#define CEC_RX_QUEUE 4
static uint8_t s_cec_q[CEC_RX_QUEUE][ADV7511_CEC_MAX_FRAME];
static uint8_t s_cec_q_len[CEC_RX_QUEUE];
static uint32_t s_cec_q_since;          // when the oldest became the oldest (the hold)
static uint8_t s_cec_q_head = 0;
static volatile uint8_t s_cec_q_n = 0;
// The SMBus relay's requests, from the I2C2 ISR: a frame to send and a received frame to
// release. The ISR may not touch I2C1 (the main loop can be mid-transfer on it, and every
// transfer stretches the console's clock), so it only records them; adv7511_cec_poll() does
// the register work. s_cec_tx_req_len != 0 = a frame waits.
static uint8_t s_cec_tx_req[ADV7511_CEC_MAX_FRAME];
static volatile uint8_t s_cec_tx_req_len = 0;
static volatile bool s_cec_rx_release = false;
static uint8_t s_cec_la = 0;   // logical address claimed (adv7511_cec_claim_step()), 0 = none
static bool s_cec_inited = false;   // adv7511_cec_init() has run this boot
static bool s_cec_tx_quiet = false; // One Touch Play is retrying a lost send: don't log each one
// Frames X-HD Redux sends on its own, oldest first, each as soon as Tx is free (see
// adv7511_cec_poll()): the answers to requests addressed to us (cec_rx_take()), the <Active
// Source> that answers <Set Stream Path> / <Request Active Source> (cec_track_route()), a new
// <Report Physical Address>, and the shutdown <Standby> (adv7511_cec_standby()). A queue, not
// one slot: TVs ask several things back to back while discovering devices, and a single slot
// lost all but the last. op is the opcode sent, to the destination, arg its one operand.
#define CEC_REPLY_QUEUE 4
static struct { uint8_t op, to, arg; } s_cec_reply[CEC_REPLY_QUEUE];
static uint8_t s_cec_reply_n = 0;
// The physical address last broadcast in <Report Physical Address> (adv7511_cec_claim_step()).
static uint16_t s_cec_pa_reported = 0xFFFF;
// Whether the TV is showing this input, for the shutdown <Standby>: assumed until a routing
// message on the bus names another physical address (see cec_track_route()).
static bool s_cec_active = true;
// <Standby> from the TV (logical address 0), for the BIOS to shut the console down cleanly (READ
// 24 bit 2). Not from other devices: a soundbar or media box can send it too. Kept until the TV
// shows it is on again -- any other frame from it, or HPD / Monitor Sense rising -- or the BIOS clears it
// (adv7511_cec_tv_standby_clear()). RAM only: every power-on starts clear.
static volatile bool s_cec_tv_standby = false;
// What One Touch Play changed, for the shutdown to put back (adv7511_cec_standby()): the TV's
// <Report Power Status> to our ask (0 on, 0xFF no answer) and the physical address of the
// source that answered <Request Active Source> (0xFFFF none) -- both before our <Active Source>.
static uint8_t s_rst_tv = 0xFF;
static uint16_t s_rst_pa = 0xFFFF;
static uint32_t s_rst_asked_at;
static bool s_rst_asked;
#define CEC_RESTORE_ASK_MS 500u
// s_claim_step: 0-2 polling k_playback_la[step], then the steps below. Image View On, Active
// Source and Set OSD Name only with One Touch Play.
enum {
    CEC_CLAIM_IMAGE_VIEW_ON = 3, CEC_CLAIM_REPORT_PA, CEC_CLAIM_ACTIVE_SOURCE, CEC_CLAIM_OSD_NAME,
    CEC_CLAIM_DONE,
};
static uint8_t s_claim_step = 0;

// Queues one of our own frames (above); dropped when the queue is full.
static void cec_reply(uint8_t op, uint8_t to, uint8_t arg) {
    if (s_cec_reply_n == CEC_REPLY_QUEUE) return;
    s_cec_reply[s_cec_reply_n].op = op;
    s_cec_reply[s_cec_reply_n].to = to;
    s_cec_reply[s_cec_reply_n].arg = arg;
    s_cec_reply_n++;
}
// How long the oldest queued frame waits for the relay before it is dropped.
#define CEC_RELAY_HOLD_MS 1000u

// <Set OSD Name> (0x47) from `from` to `to`: the SPD product name (page 63, bytes 8-23),
// trailing padding trimmed, at most 14 characters (CEC's limit). The SPD vendor has no CEC
// counterpart -- <Device Vendor ID> is an IEEE OUI, not text. Returns the frame length.
static uint8_t cec_osd_name_frame(uint8_t *frame, uint8_t from, uint8_t to) {
    const uint8_t *product = user_config_get_spd() + 8;
    uint8_t n = 14;
    while (n > 1 && product[n - 1] == ' ') n--;
    frame[0] = (uint8_t)((from << 4) | to);
    frame[1] = CEC_OP_SET_OSD_NAME;
    memcpy(&frame[2], product, n);
    return (uint8_t)(n + 2);
}

// Broadcast from our claimed address with a physical address: <Active Source> (0x82) or
// <Report Physical Address> (0x84, plus its device type: playback). Returns the length.
static uint8_t cec_pa_frame(uint8_t *frame, uint8_t op, uint16_t pa) {
    frame[0] = (uint8_t)((s_cec_la << 4) | CEC_LA_BROADCAST);
    frame[1] = op;
    frame[2] = (uint8_t)(pa >> 8);
    frame[3] = (uint8_t)pa;
    frame[4] = CEC_DEVICE_PLAYBACK;
    return (op == CEC_OP_REPORT_PHYSICAL_ADDR) ? 5 : 4;
}

// Frees Rx buffer `b` (0-2): its Ready Clear bit, set then cleared -- a bare write of 1 left
// standing doesn't reliably re-arm it (see the header comment).
static void cec_rx_clear(uint8_t b) {
    const uint8_t bit = (uint8_t)(1u << b);
    adv7511_cec_update_register(0x4A, bit, bit);
    adv7511_cec_update_register(0x4A, bit, 0x00);
}

// Programs the claimed logical address into the chip's address slot 0 (0x4C[3:0]) and enables
// that slot (0x4B[4]) -- the Linux driver sets the enable explicitly rather than trusting its
// power-on value.
static void cec_set_la(void) {
    adv7511_cec_update_register(0x4C, 0x0F, s_cec_la);
    adv7511_cec_update_register(0x4B, 0x10, 0x10);
}

void adv7511_cec_init(void) {
    // Main Map: assign the CEC Map I2C sub-address, then power up the CEC section (0xE2[0]
    // = 0 -- active-low power-down bit, per Programming Guide Table 85).
    adv7511_write_register(0xE1, ADV7511_CEC_I2C_ADDR);
    adv7511_update_register(0xE2, 0x01, 0x00);
    // Soft reset (0x50[0] set, then clear): the guide's (Sec 4.8.6) and the Linux driver's
    // init both start the CEC block from it.
    adv7511_cec_write_register(0x50, 0x01);
    adv7511_cec_write_register(0x50, 0x00);
    // No send in flight (Linux driver: "initially disable tx"). A Tx enable left from before a
    // clock outage would otherwise go out stale the moment the clock returns.
    adv7511_cec_write_register(0x11, 0x00);
    // The soft reset forgets the claimed logical address (0x4C[3:0] back to 0xF).
    if (s_cec_la) cec_set_la();
    // Tx Retry (0x12[6:4]): 3, so 4 attempts, as the Linux CEC core asks for; the chip's
    // default is 1 (CEC allows up to 5).
    adv7511_cec_update_register(0x12, 0x70, 0x30);
    // CEC Map: power up the CEC section itself, 0b01 = Always Active (Table 86).
    adv7511_cec_update_register(0x4E, 0x03, 0x01);
    // All three Rx buffers, emptied (the Linux driver's 0x0F then 0x08) -- see the header comment.
    adv7511_cec_write_register(0x4A, 0x0F);
    adv7511_cec_write_register(0x4A, 0x08);
    // No Tx in flight and nothing queued for the relay yet this boot/HPD-reset.
    s_cec_tx_status = 0;
    s_cec_q_n = 0;
    s_cec_inited = true;
}

// "[CEC] <what> AA BB ... <tail>" as one debug_log(): the frame is hex-formatted here
// because the log's formatter has no per-byte loop.
static void cec_log(const char *what, const uint8_t *frame, uint8_t len, const char *tail) {
    char hex[ADV7511_CEC_MAX_FRAME * 3 + 1];
    for (uint8_t i = 0; i < len; i++) {
        hex[i * 3] = ' ';
        hex[i * 3 + 1] = "0123456789ABCDEF"[frame[i] >> 4];   // the literal mini_vsnprintf uses:
        hex[i * 3 + 2] = "0123456789ABCDEF"[frame[i] & 0x0F]; // one copy in the image
    }
    hex[len * 3] = '\0';
    debug_log(LOG_CEC_MSG, what, hex, tail);
}

// Active-source tracking off the broadcast routing messages: <Active Source> [PA],
// <Routing Information> [PA], <Set Stream Path> [PA] and <Routing Change> [old PA][new PA]
// name the path the TV now shows; we are active only if that is our physical address.
// <Set Stream Path> to our address is the TV selecting us: answer <Active Source>. Only what
// the bus says -- a switch or TV input change that sends no CEC goes unseen. <Request Active
// Source> from the TV (it asks after waking) is answered the same way while we are shown;
// libCEC answers only the TV too, so a request from, say, a receiver can't wake the TV.
static void cec_track_route(const uint8_t *f, uint8_t op, uint8_t len) {
    if (op == CEC_OP_REQUEST_ACTIVE_SOURCE) {
        if (s_cec_la && s_cec_active && (f[0] >> 4) == CEC_LA_TV) cec_reply(CEC_OP_ACTIVE_SOURCE, CEC_LA_BROADCAST, 0);
        return;
    }
    uint8_t at;   // where the physical address that is now shown starts
    if (op == CEC_OP_ACTIVE_SOURCE || op == CEC_OP_ROUTING_INFORMATION || op == CEC_OP_SET_STREAM_PATH) {
        at = 2;
    } else if (op == CEC_OP_ROUTING_CHANGE) {   // [old PA][new PA]
        at = 4;
    } else {
        return;
    }
    if (len < at + 2) return;
    // Another source's <Active Source> before ours went out: the one to go back to.
    if (op == CEC_OP_ACTIVE_SOURCE && s_claim_step <= CEC_CLAIM_ACTIVE_SOURCE && (f[0] >> 4) != s_cec_la)
        s_rst_pa = (uint16_t)((f[2] << 8) | f[3]);
    const bool active =
        (uint16_t)((f[at] << 8) | f[at + 1]) == edid_cec_physical_address();
    if (active != s_cec_active) debug_log(LOG_CEC_ACTIVE_SOURCE, active);
    s_cec_active = active;
    if (active && op == CEC_OP_SET_STREAM_PATH && s_cec_la) cec_reply(CEC_OP_ACTIVE_SOURCE, CEC_LA_BROADCAST, 0);
}

bool adv7511_cec_tx(const uint8_t *frame, uint8_t len) {
    if (len == 0 || len > ADV7511_CEC_MAX_FRAME) return false;
    if (s_cec_tx_status & ADV7511_CEC_TX_BUSY) return false;
    if (!s_cec_tx_quiet) cec_log("tx", frame, len, "");
    for (uint8_t i = 0; i < len; i++) {
        adv7511_cec_write_register(i, frame[i]); // 0x00 (header) .. up to 0x0F
    }
    adv7511_cec_write_register(0x10, len);
    s_cec_tx_status = ADV7511_CEC_TX_BUSY;
    adv7511_cec_update_register(0x11, 0x01, 0x01); // Tx Enable -- kicks off the send
    return true;
}

static const uint8_t k_cec_rx_hdr[3] = { 0x15, 0x27, 0x38 };   // + 0x10: the length

// Requests addressed to us that X-HD Redux answers itself, and the frame each gets (operand in
// cec_reply()'s arg): what TVs ask while listing a device, and what some need before they
// pass remote keys on (<Menu Request>; LG and Samsung by libCEC). <Give Device Vendor ID>
// gets <Feature Abort>: CEC's vendor ID is an IEEE OUI X-HD Redux doesn't have (libCEC does
// the same without one).
static const uint8_t k_cec_answer[][3] = {
    { 0x46, 0x47, 0    },   // <Give OSD Name> -> <Set OSD Name>
    { 0x83, 0x84, 0    },   // <Give Physical Address> -> <Report Physical Address> (broadcast)
    { 0x8C, 0x00, 0x8C },   // <Give Device Vendor ID> -> <Feature Abort>
    { 0x8F, 0x90, 0x00 },   // <Give Device Power Status> -> <Report Power Status> on
    { 0x9F, 0x9E, 0x06 },   // <Get CEC Version> -> <CEC Version> 2.0
    { 0x8D, 0x8E, 0x00 },   // <Menu Request> -> <Menu Status> activated
    { 0xA5, 0xA6, 0    },   // <Give Features> -> <Report Features> (broadcast)
};
// <Report Features> operands (CEC 2.0): version 2.0, playback, RC profile "source" handling the
// root, setup, contents, media top and media context menus -- some TVs pass a source only the
// menu keys its profile lists -- and no optional device features.
static const uint8_t k_cec_features[4] = { 0x06, 0x10, 0x5F, 0x00 };
// Frames addressed to us that need no answer (replies, key presses, <Standby>, ...). Any other
// one gets <Feature Abort> [opcode, unrecognized], as CEC requires, and still goes to the relay.
// <Play> and <Deck Control> are a playback device's transport keys: libCEC doesn't abort them.
// The broadcast-only messages (0x32, 0x80-0x87, 0xA6-0xA8) sent to us directly have the wrong
// addressing mode, which CEC says to ignore: no abort (and cec_track_route() skips them).
static const uint8_t k_cec_no_abort[] = {
    0x00, 0x04, 0x0D, 0x1B, 0x32, 0x36, 0x41, 0x42, 0x44, 0x45, 0x47, 0x72, 0x7A, 0x7E,
    0x80, 0x81, 0x82, 0x84, 0x85, 0x86, 0x87, 0x8A, 0x8B, 0x8E, 0x90, 0x9D, 0x9E, 0xA6,
    0xA7, 0xA8,
};

// Copies Rx buffer `b` out and frees it. A request in k_cec_answer to our claimed address
// (adv7511_cec_claim_step(); without one the chip takes no directed frames) is answered and
// kept off the relay; everything else queues for it.
static void cec_rx_take(uint8_t b) {
    const uint8_t base = k_cec_rx_hdr[b];
    const uint8_t len = adv7511_cec_read_register((uint8_t)(base + 0x10)) & 0x1F;
    if (len == 0 || len > ADV7511_CEC_MAX_FRAME) {
        cec_rx_clear(b);   // impossible length: never queued
        return;
    }
    if (s_cec_q_n == CEC_RX_QUEUE) adv7511_cec_rx_ack();   // full: drop the oldest
    const uint8_t slot = (uint8_t)((s_cec_q_head + s_cec_q_n) % CEC_RX_QUEUE);
    uint8_t *f = s_cec_q[slot];
    for (uint8_t i = 0; i < len; i++) {
        f[i] = adv7511_cec_read_register((uint8_t)(base + i));
    }
    cec_rx_clear(b);
    const uint8_t op = (len >= 2) ? f[1] : 0;
    if ((f[0] & 0x0F) == CEC_LA_BROADCAST) cec_track_route(f, op, len);   // broadcasts only (above)
    const bool to_us = s_cec_la && len >= 2 && (f[0] & 0x0F) == s_cec_la;
    const uint8_t from = (uint8_t)(f[0] >> 4);
    if (from == CEC_LA_TV) s_cec_tv_standby = (op == CEC_OP_STANDBY);
    if (from == CEC_LA_TV && op == CEC_OP_REPORT_POWER_STATUS && len >= 3) s_rst_tv = f[2];
    if (to_us) {
        for (uint8_t i = 0; i < sizeof k_cec_answer / sizeof k_cec_answer[0]; i++) {
            if (op == k_cec_answer[i][0]) {
                cec_log("rx", f, len, " ans");
                // From Unregistered (15) only the broadcast answer goes out: a directed one
                // would have nobody to go to (the Linux CEC core does the same).
                if (from != CEC_LA_UNREGISTERED || k_cec_answer[i][1] == CEC_OP_REPORT_PHYSICAL_ADDR ||
                    k_cec_answer[i][1] == CEC_OP_REPORT_FEATURES) {
                    cec_reply(k_cec_answer[i][1], from, k_cec_answer[i][2]);
                }
                return;
            }
        }
        if (from != CEC_LA_UNREGISTERED && !memchr(k_cec_no_abort, op, sizeof k_cec_no_abort)) {
            cec_reply(CEC_OP_FEATURE_ABORT, from, op);
        }
    }
    cec_log("rx", f, len, " fwd");
    s_cec_q_len[slot] = len;
    if (s_cec_q_n == 0) s_cec_q_since = HAL_GetTick();
    __DMB();   // the frame is in place before the ISR can see it queued
    s_cec_q_n++;
}

void adv7511_cec_poll(void) {
    // The relay's requests first (see s_cec_tx_req): the release frees the receiver for the
    // read below, and the send goes out ahead of our own replies.
    if (s_cec_rx_release) {
        adv7511_cec_rx_ack();
    }
    if (s_cec_tx_req_len && !(s_cec_tx_status & ADV7511_CEC_TX_BUSY)) {
        ISR_BARRIER();
        adv7511_cec_tx(s_cec_tx_req, s_cec_tx_req_len);
        s_cec_tx_req_len = 0;
    }
    // The oldest of our own queued frames (s_cec_reply) goes out as soon as Tx is free.
    if (s_cec_reply_n && !(s_cec_tx_status & ADV7511_CEC_TX_BUSY)) {
        uint8_t frame[ADV7511_CEC_MAX_FRAME];
        uint8_t len;
        const uint8_t op = s_cec_reply[0].op;
        if (op == CEC_OP_SET_OSD_NAME) {
            len = cec_osd_name_frame(frame, s_cec_la, s_cec_reply[0].to);
        } else if (op == CEC_OP_REPORT_PHYSICAL_ADDR || op == CEC_OP_ACTIVE_SOURCE ||
                   op == CEC_OP_SET_STREAM_PATH || op == CEC_OP_INACTIVE_SOURCE) {
            // <Report Physical Address>: the one adv7511_cec_claim_step() last saw (it follows
            // the EDID); <Set Stream Path>: the source to go back to; <Active Source> and
            // <Inactive Source> (to the TV): the EDID's.
            len = cec_pa_frame(frame, op, op == CEC_OP_REPORT_PHYSICAL_ADDR ? s_cec_pa_reported :
                                          op == CEC_OP_SET_STREAM_PATH ? s_rst_pa : edid_cec_physical_address());
            if (op == CEC_OP_INACTIVE_SOURCE) frame[0] = (uint8_t)((s_cec_la << 4) | CEC_LA_TV);
        } else if (op == CEC_OP_REPORT_FEATURES) {
            frame[0] = (uint8_t)((s_cec_la << 4) | CEC_LA_BROADCAST);
            frame[1] = op;
            memcpy(&frame[2], k_cec_features, sizeof k_cec_features);
            len = 6;
        } else {
            // <Standby> (2 bytes; from 15, unregistered, without a claimed address), <Feature
            // Abort> [opcode, reason] (4: 0 unrecognized, 4 refused -- CEC's answer to
            // <Abort>), or a one-operand answer (3).
            frame[0] = (uint8_t)(((s_cec_la ? s_cec_la : CEC_LA_UNREGISTERED) << 4) | s_cec_reply[0].to);
            frame[1] = op;
            frame[2] = s_cec_reply[0].arg;
            frame[3] = (frame[2] == CEC_OP_ABORT) ? CEC_ABORT_REFUSED : CEC_ABORT_UNRECOGNIZED;
            len = (op == CEC_OP_STANDBY || op == CEC_OP_GIVE_POWER_STATUS || op == CEC_OP_REQUEST_ACTIVE_SOURCE) ? 2 :
                  (op == CEC_OP_FEATURE_ABORT) ? 4 : 3;
        }
        if (adv7511_cec_tx(frame, len)) {
            s_cec_reply_n--;
            for (uint8_t i = 0; i < s_cec_reply_n; i++) s_cec_reply[i] = s_cec_reply[i + 1];
        }
    }
    if (s_cec_tx_status & ADV7511_CEC_TX_BUSY) {
        // Main Map 0x97[5:3]: Tx Ready(success) / Arbitration Lost / Retry Timeout -- the
        // three (mutually exclusive) ways a send can finish. Read+clear these bits
        // ourselves, here, rather than leaving them for adv_handle_interrupts()'s own
        // generic 0x97 read-and-clear-everything (the supervisor calls this first each pass)
        // -- otherwise a CEC completion landing on the same shared INT-pin edge as an
        // unrelated HPD/PLL event could get silently absorbed by that generic clear before
        // this function ever saw it.
        uint8_t status = adv7511_read_register(0x97) & 0b00111000;
        if (status != 0) {
            adv7511_write_register(0x97, status); // write-1-to-clear, only the bits we read
            adv7511_cec_update_register(0x11, 0x01, 0x00); // Tx Enable must be cleared
                                                             // before the next send (Guide
                                                             // Sec 4.8.4.1, Step 4)
            s_cec_tx_status = (uint8_t)((status & 0b00100000) ? ADV7511_CEC_TX_OK :
                               (status & 0b00010000) ? ADV7511_CEC_TX_ARB_LOST :
                                                        ADV7511_CEC_TX_TIMEOUT);
        }
    }
    if (s_cec_q_n && (HAL_GetTick() - s_cec_q_since) >= CEC_RELAY_HOLD_MS) {
        debug_log(LOG_CEC_RX_DROPPED);
        adv7511_cec_rx_ack();
    }
    // Every frame the chip holds, oldest first (0x26: two bits a buffer, 1 = oldest).
    const uint8_t ready = adv7511_cec_read_register(0x49) & 0x07;
    if (ready) {
        const uint8_t stamps = adv7511_cec_read_register(0x26);
        for (uint8_t stamp = 1; stamp <= 3; stamp++) {
            for (uint8_t b = 0; b < 3; b++) {
                if (((stamps >> (2 * b)) & 3) == stamp && (ready & (1u << b))) cec_rx_take(b);
            }
        }
    }
}

// Claiming a CEC address, every boot, and One Touch Play when it is on: claim a Playback
// logical address (poll 4, 8, 11; the first nobody ACKs is free) -- without one the chip takes
// no frames addressed to us, so no remote keys and no answers -- then [One Touch Play: send
// <Image View On> to the TV (wakes it)], then, once the TV's EDID gives our physical address,
// broadcast <Report Physical Address> (CEC requires it after a claim; TVs list devices from
// it; CEC 2.0's <Report Features> is queued behind it), then [One Touch Play: broadcast
// <Active Source> (the TV switches to this input) and send the TV our <Set OSD Name> (the SPD
// product; device lists show it)]. Starts at boot whatever HPD says: a TV in standby drops
// HPD but still listens on CEC, so waiting for HPD never woke it. One step per completed send, so it never blocks; a result-less status (a re-init,
// arbitration lost) resends the same step. Arbitration lost waits a second first, is logged
// once, and gives up for the boot after 5 resends in a row: a CEC line held low (no CEC sink
// pulling it up) loses every send at once, and resending each pass flooded the UART. Once
// done, a new physical address (another HDMI port) starts the claim again, without One Touch
// Play.
static const uint8_t k_playback_la[3] = { 4, 8, 11 };
static bool s_claim_sent = false;
static uint32_t s_claim_lost_at = 0;
static uint8_t s_claim_lost = 0;   // arbitration losses in a row
static bool s_cec_reclaim = false; // claiming again after a physical address change
void adv7511_cec_claim_step(uint16_t phys_addr, bool one_touch) {
    if (s_claim_step >= CEC_CLAIM_DONE) {
        if (!s_cec_la || phys_addr == 0xFFFF || phys_addr == s_cec_pa_reported) return;
        // A new physical address: claim again from the start -- CEC allocates the logical
        // address per physical address (Linux does the same), and another device there may
        // hold ours. The old one is given up first, with any reply still queued from it (it
        // would go out from 0, the TV); no One Touch Play on a moved cable.
        s_cec_la = 0;
        s_cec_reply_n = 0;
        adv7511_cec_update_register(0x4B, 0x10, 0x00);
        s_claim_step = 0;
        s_claim_lost = 0;
        s_cec_reclaim = true;
    }
    if (s_cec_reclaim) one_touch = false;
    // The chip's own setup (and its CEC init) waits for HPD; CEC doesn't need it, and its
    // registers (0xE1/0xE2, the CEC Map) sit outside what an HPD drop resets.
    if (!s_cec_inited) adv7511_cec_init();
    if (s_claim_sent) {
        const uint8_t st = s_cec_tx_status;
        if (st & ADV7511_CEC_TX_BUSY) return;
        s_claim_sent = false;
        if (st & ADV7511_CEC_TX_ARB_LOST) {
            if (++s_claim_lost > 5) {
                debug_log(LOG_CEC_ARB_LOST_GIVING);
                s_claim_step = CEC_CLAIM_DONE;
                s_cec_tx_quiet = false;
                return;
            }
            if (!s_cec_tx_quiet) debug_log(LOG_CEC_ARB_LOST_RETRYING);
            s_cec_tx_quiet = true;
            s_claim_lost_at = HAL_GetTick();
        }
        if (st & (ADV7511_CEC_TX_OK | ADV7511_CEC_TX_TIMEOUT)) {
            s_cec_tx_quiet = false;
            s_claim_lost = 0;
            if (s_claim_step >= CEC_CLAIM_IMAGE_VIEW_ON) {
                if (s_claim_step == CEC_CLAIM_REPORT_PA) {    // CEC 2.0: features after the address
                    cec_reply(CEC_OP_REPORT_FEATURES, CEC_LA_BROADCAST, 0);
                }
                s_claim_step++;
            } else if (st & ADV7511_CEC_TX_TIMEOUT) {         // no ACK: the address is free
                s_cec_la = k_playback_la[s_claim_step];
                cec_set_la();
                s_claim_step = CEC_CLAIM_IMAGE_VIEW_ON;
            } else if (++s_claim_step == CEC_CLAIM_IMAGE_VIEW_ON) {   // all three taken: give up
                s_claim_step = CEC_CLAIM_DONE;
            }
        }
    }
    if (!one_touch && (s_claim_step == CEC_CLAIM_IMAGE_VIEW_ON || s_claim_step == CEC_CLAIM_ACTIVE_SOURCE)) {
        s_claim_step = (s_claim_step == CEC_CLAIM_IMAGE_VIEW_ON) ? CEC_CLAIM_REPORT_PA : CEC_CLAIM_DONE;
    }
    if (s_claim_step >= CEC_CLAIM_DONE) return;
    if (s_cec_tx_quiet && (HAL_GetTick() - s_claim_lost_at) < 1000u) return;
    uint8_t frame[ADV7511_CEC_MAX_FRAME];
    uint8_t len;
    if (s_claim_step < CEC_CLAIM_IMAGE_VIEW_ON) {
        frame[0] = (uint8_t)(k_playback_la[s_claim_step] * 0x11); // polling: from and to the candidate
        len = 1;
    } else if (s_claim_step == CEC_CLAIM_IMAGE_VIEW_ON) {
        // First what the shutdown should put back: the TV's power status and who is shown --
        // the answers have CEC_RESTORE_ASK_MS, before our <Active Source> replaces the source.
        if (!s_rst_asked) {
            s_rst_asked = true;
            s_rst_asked_at = HAL_GetTick();
            cec_reply(CEC_OP_GIVE_POWER_STATUS, CEC_LA_TV, 0);
            cec_reply(CEC_OP_REQUEST_ACTIVE_SOURCE, CEC_LA_BROADCAST, 0);
        }
        if (HAL_GetTick() - s_rst_asked_at < CEC_RESTORE_ASK_MS) return;
        frame[0] = (uint8_t)((s_cec_la << 4) | CEC_LA_TV);
        frame[1] = CEC_OP_IMAGE_VIEW_ON;
        len = 2;
    } else if (s_claim_step == CEC_CLAIM_OSD_NAME) {
        len = cec_osd_name_frame(frame, s_cec_la, CEC_LA_TV);
    } else {
        if (phys_addr == 0xFFFF) return;                     // no EDID yet: TV still waking
        const bool report = (s_claim_step == CEC_CLAIM_REPORT_PA);   // else <Active Source>
        len = cec_pa_frame(frame, report ? CEC_OP_REPORT_PHYSICAL_ADDR : CEC_OP_ACTIVE_SOURCE, phys_addr);
        if (report) {
            s_cec_pa_reported = phys_addr;
        } else {
            s_cec_active = true;
        }
    }
    s_claim_sent = adv7511_cec_tx(frame, len);
}

// <Standby> to the TV (console shutting down), sent by adv7511_cec_poll() once Tx is free.
// Replaces whatever else is queued: nothing there matters with the console going off. From
// our claimed address, or 15 (unregistered) without one. Skipped when the bus said the TV
// shows another input (cec_track_route()): don't turn off what's in use; and when the TV
// started the shutdown (s_cec_tv_standby): it is already off.
void adv7511_cec_standby(bool always) {
    if (!s_cec_active || s_cec_tv_standby) {
        debug_log(LOG_CEC_STANDBY_SKIPPED);   // why: the earlier rx / active source lines
        return;
    }
    s_cec_reply_n = 0;
    // Put back what One Touch Play changed: a TV that was on goes back to the source shown
    // then (<Set Stream Path> to it), or with none known we step down (<Inactive Source>);
    // one in standby, or that never answered, goes back to standby. `always` (the Always
    // Standby setting, cec_standby_off == 2) skips the restore and turns the TV off outright.
    if (!always && s_rst_tv == 0) {
        cec_reply(s_rst_pa != 0xFFFF ? CEC_OP_SET_STREAM_PATH : CEC_OP_INACTIVE_SOURCE, CEC_LA_TV, 0);
    } else {
        cec_reply(CEC_OP_STANDBY, CEC_LA_TV, 0);
    }
}

bool adv7511_cec_tv_standby(void) {
    return s_cec_tv_standby;
}

// ISR-safe: HPD / Monitor Sense rising (the TV is on) or the BIOS (WRITE_CEC_RX_ACK=2) clears the flag.
void adv7511_cec_tv_standby_clear(void) {
    s_cec_tv_standby = false;
}

// ISR-safe (the SMBus relay): queues one frame for adv7511_cec_poll() to send. false, like
// adv7511_cec_tx(), for a bad length or while a send is in flight or already queued.
bool adv7511_cec_tx_queue(const uint8_t *frame, uint8_t len) {
    if (len == 0 || len > ADV7511_CEC_MAX_FRAME) return false;
    if (s_cec_tx_req_len || (s_cec_tx_status & ADV7511_CEC_TX_BUSY)) return false;
    memcpy(s_cec_tx_req, frame, len);
    ISR_BARRIER();
    s_cec_tx_req_len = len;
    return true;
}

// ISR-safe: the relay is done with the oldest frame; adv7511_cec_poll() drops it from the
// queue. It reads as gone at once, so the BIOS can't read it twice.
void adv7511_cec_rx_release(void) {
    if (s_cec_q_n) s_cec_rx_release = true;
}

// A queued relay send reads as in flight until it has gone out and finished.
uint8_t adv7511_cec_tx_status(void) {
    return s_cec_tx_req_len ? ADV7511_CEC_TX_BUSY : s_cec_tx_status;
}

bool adv7511_cec_rx_pending(uint8_t *len) {
    if (!s_cec_q_n || s_cec_rx_release) return false;
    if (len) *len = s_cec_q_len[s_cec_q_head];
    return true;
}

void adv7511_cec_rx_read(uint8_t *out) {
    if (!s_cec_q_n || s_cec_rx_release || !out) return;
    memcpy(out, s_cec_q[s_cec_q_head], s_cec_q_len[s_cec_q_head]);
}

// Main loop: drops the oldest queued frame (the chip's buffers are already free).
void adv7511_cec_rx_ack(void) {
    s_cec_rx_release = false;
    if (!s_cec_q_n) return;
    s_cec_q_head = (uint8_t)((s_cec_q_head + 1) % CEC_RX_QUEUE);
    ISR_BARRIER();   // the ISR reads the head whenever s_cec_q_n says a frame is there
    s_cec_q_n--;
    s_cec_q_since = HAL_GetTick();   // the next frame's hold starts now
}
#endif
