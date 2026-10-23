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
    I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();
    HAL_I2C_Mem_Read(hi2c, ADV7511_MAIN_I2C_ADDR, address, I2C_MEMADD_SIZE_8BIT, &data, 1, 20);
    return data;
}

void adv7511_write_register(const uint8_t address, uint8_t value) {
    I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();
    HAL_I2C_Mem_Write(hi2c, ADV7511_MAIN_I2C_ADDR, address, I2C_MEMADD_SIZE_8BIT, &value, 1, 20);
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
void adv7511_ensure_powered_up(void) {
    if (adv7511_read_register(0x41) & 0x40) {
        adv7511_update_register(0x41, 0b01000000, 0b00000000);
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
    // single-byte I2C transactions.
    HAL_I2C_Mem_Write(adv7511_i2c_instance(), ADV7511_MAIN_I2C_ADDR, 0x18,
                      I2C_MEMADD_SIZE_8BIT, (uint8_t *)coefficients, 24, 20);
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
    I2C_HandleTypeDef *hi2c = adv7511_i2c_instance();
    HAL_I2C_Mem_Read(hi2c, ADV7511_MAIN_I2C_ADDR, 0x04, I2C_MEMADD_SIZE_8BIT, cts_buf, 3, 20);
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
// CLI/GUI show a new clock 100ms sooner than the classifiers act on it. Classifiers keep
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
static uint8_t adv7511_cec_read_register(const uint8_t address) {
    uint8_t data = 0;
    I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();
    HAL_I2C_Mem_Read(hi2c, ADV7511_CEC_I2C_ADDR, address, I2C_MEMADD_SIZE_8BIT, &data, 1, 20);
    return data;
}

static void adv7511_cec_write_register(const uint8_t address, uint8_t value) {
    I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();
    HAL_I2C_Mem_Write(hi2c, ADV7511_CEC_I2C_ADDR, address, I2C_MEMADD_SIZE_8BIT, &value, 1, 20);
}

static void adv7511_cec_update_register(const uint8_t address, const uint8_t mask, uint8_t new_value) {
    const uint8_t current = adv7511_cec_read_register(address);
    uint8_t updated = (current & ~mask) | (new_value & mask);
    adv7511_cec_write_register(address, updated);
}

// Register map verified two ways before writing any of this: the ADV7511W Programming
// Guide's own CEC section (Tables 78/79/85/86 -- Tx frame 0x00/0x01-0x0F/0x10/0x11, Rx
// Buffer 1 frame 0x15/0x16-0x24/0x25, Rx ready/clear 0x49/0x4A[2:0]) and, since the guide's
// prose was ambiguous on a couple of exact semantics, cross-checked against the Linux
// adv7511 CEC driver for anything the prose didn't spell out precisely:
//   - Tx frame length (0x10) is the LITERAL total byte count (header + data), not
//     operands+2 or len-1 -- confirmed by the driver's own `regmap_write(..., len)` with a
//     comment reading "set length (data + header)".
//   - Rx Buffer 1's "Ready Clear" bit (0x4A[0]) needs a momentary set-then-clear pulse to
//     actually re-arm the buffer, not a bare write of 1 left standing -- the driver does
//     exactly that (write BIT set, then write it cleared again immediately after) even
//     though the register table's own prose reads like a plain "1 = clear out" bit.
//
// Deliberately single-buffer/legacy mode (0x4A[3] left 0, not the Linux driver's
// non-legacy/3-buffer choice) -- this is a relay for a custom BIOS's own CEC stack, not a
// general-purpose CEC device that needs to survive several messages arriving faster than
// the host can poll. One buffer keeps the poll/ack logic below a single flat state instead
// of tracking 3 buffers' timestamps, at the cost of that (rare, low-bandwidth-bus) edge case.
static uint8_t s_cec_tx_status = 0;
static volatile bool s_cec_rx_pending = false;
static uint8_t s_cec_rx_len = 0;
// The SMBus relay's requests, from the I2C2 ISR: a frame to send and a received frame to
// release. The ISR may not touch I2C1 (the main loop can be mid-transfer on it, and every
// transfer stretches the console's clock), so it only records them; adv7511_cec_poll() does
// the register work. s_cec_tx_req_len != 0 = a frame waits.
static uint8_t s_cec_tx_req[ADV7511_CEC_MAX_FRAME];
static volatile uint8_t s_cec_tx_req_len = 0;
static volatile bool s_cec_rx_release = false;
static uint8_t s_cec_rx_buf[ADV7511_CEC_MAX_FRAME];
static uint8_t s_cec_la = 0;   // logical address One Touch Play claimed, 0 = none
static bool s_cec_inited = false;   // adv7511_cec_init() has run this boot
static bool s_cec_tx_quiet = false; // One Touch Play is retrying a lost send: don't log each one
// A request addressed to our claimed logical address that X-HD answers itself (see
// adv7511_cec_poll()): its opcode and the asker's logical address; 0 = none queued. Also
// carries our own <Standby> (0x36, see adv7511_cec_standby()) and the <Active Source> (0x82)
// that answers <Set Stream Path> (cec_track_route()).
static uint8_t s_cec_reply_op = 0;
// Whether the TV is showing this input, for the shutdown <Standby>: assumed until a routing
// message on the bus names another physical address (see cec_track_route()).
static bool s_cec_active = true;
static uint8_t s_cec_reply_to = 0;
// When the frame now in the relay arrived. The receiver is single-buffered, so a frame nobody
// collects (no BIOS CEC stack) would block reception for good: dropped after a second.
static uint32_t s_cec_rx_since = 0;
#define CEC_RELAY_HOLD_MS 1000u

// <Set OSD Name> (0x47) from `from` to `to`: the SPD product name (page 63, bytes 8-23),
// trailing padding trimmed, at most 14 characters (CEC's limit). The SPD vendor has no CEC
// counterpart -- <Device Vendor ID> is an IEEE OUI, not text. Returns the frame length.
static uint8_t cec_osd_name_frame(uint8_t *frame, uint8_t from, uint8_t to) {
    const uint8_t *product = user_config_get_spd() + 8;
    uint8_t n = 14;
    while (n > 1 && product[n - 1] == ' ') n--;
    frame[0] = (uint8_t)((from << 4) | to);
    frame[1] = 0x47;
    memcpy(&frame[2], product, n);
    return (uint8_t)(n + 2);
}

// Broadcast from our claimed address with a physical address: <Active Source> (0x82) or
// <Report Physical Address> (0x84, plus its device type: playback). Returns the length.
static uint8_t cec_pa_frame(uint8_t *frame, uint8_t op, uint16_t pa) {
    frame[0] = (uint8_t)((s_cec_la << 4) | 0x0F);
    frame[1] = op;
    frame[2] = (uint8_t)(pa >> 8);
    frame[3] = (uint8_t)pa;
    frame[4] = 0x04;
    return (op == 0x84) ? 5 : 4;
}

// Rx Buffer 1 ready-clear: a set-then-clear pulse -- see this block's own header comment on
// why a bare write of 1 left standing doesn't reliably re-arm the buffer.
static void cec_rx_clear(void) {
    adv7511_cec_update_register(0x4A, 0x01, 0x01);
    adv7511_cec_update_register(0x4A, 0x01, 0x00);
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
    if (s_cec_la) adv7511_cec_update_register(0x4C, 0x0F, s_cec_la);
    // CEC Map: power up the CEC section itself, 0b01 = Always Active (Table 86).
    adv7511_cec_update_register(0x4E, 0x03, 0x01);
    // Legacy single-buffer Rx mode -- see this block's own header comment.
    adv7511_cec_update_register(0x4A, 0x08, 0x00);
    // No Tx in flight and no Rx buffered yet this boot/HPD-reset.
    s_cec_tx_status = 0;
    s_cec_rx_pending = false;
    s_cec_rx_len = 0;
    s_cec_inited = true;
}

// "[CEC] <what> AA BB ... <tail>" as one debug_log(): the frame is hex-formatted here
// because the log's formatter has no per-byte loop.
static void cec_log(const char *what, const uint8_t *frame, uint8_t len, const char *tail) {
    static const char hexd[] = "0123456789ABCDEF";
    char hex[ADV7511_CEC_MAX_FRAME * 3 + 1];
    for (uint8_t i = 0; i < len; i++) {
        hex[i * 3] = ' ';
        hex[i * 3 + 1] = hexd[frame[i] >> 4];
        hex[i * 3 + 2] = hexd[frame[i] & 0x0F];
    }
    hex[len * 3] = '\0';
    debug_log("[CEC] %s%s%s\r\n", what, hex, tail);
}

// Active-source tracking off the broadcast routing messages: <Active Source> [PA],
// <Routing Information> [PA], <Set Stream Path> [PA] and <Routing Change> [old PA][new PA]
// name the path the TV now shows; we are active only if that is our physical address.
// <Set Stream Path> to our address is the TV selecting us: answer <Active Source>. Only what
// the bus says -- a switch or TV input change that sends no CEC goes unseen.
static void cec_track_route(uint8_t op, uint8_t len) {
    uint8_t at;
    if (op == 0x82 || op == 0x81 || op == 0x86) {
        at = 2;
    } else if (op == 0x80) {
        at = 4;
    } else {
        return;
    }
    if (len < at + 2) return;
    const bool active =
        (uint16_t)((s_cec_rx_buf[at] << 8) | s_cec_rx_buf[at + 1]) == edid_cec_physical_address();
    if (active != s_cec_active) debug_log("[CEC] active source: %u\r\n", active);
    s_cec_active = active;
    if (active && op == 0x86 && s_cec_la) s_cec_reply_op = 0x82;
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

void adv7511_cec_poll(void) {
    // The relay's requests first (see s_cec_tx_req): the release frees the receiver for the
    // read below, and the send goes out ahead of our own replies.
    if (s_cec_rx_release) {
        adv7511_cec_rx_ack();
    }
    if (s_cec_tx_req_len && !(s_cec_tx_status & ADV7511_CEC_TX_BUSY)) {
        adv7511_cec_tx(s_cec_tx_req, s_cec_tx_req_len);
        s_cec_tx_req_len = 0;
    }
    // A queued reply (see the Rx path below) or <Standby> (adv7511_cec_standby()) goes out
    // as soon as Tx is free.
    if (s_cec_reply_op && !(s_cec_tx_status & ADV7511_CEC_TX_BUSY)) {
        uint8_t frame[ADV7511_CEC_MAX_FRAME];
        uint8_t len;
        if (s_cec_reply_op == 0x46) {
            len = cec_osd_name_frame(frame, s_cec_la, s_cec_reply_to);
        } else if (s_cec_reply_op == 0x83 || s_cec_reply_op == 0x82) {
            // <Give Physical Address> -> <Report Physical Address>; or our <Active Source>
            len = cec_pa_frame(frame, s_cec_reply_op == 0x83 ? 0x84 : 0x82,
                               edid_cec_physical_address());
        } else if (s_cec_reply_op == 0x36) {              // <Standby>, to the TV
            frame[0] = (uint8_t)((s_cec_la ? s_cec_la : 0x0F) << 4);
            frame[1] = 0x36;
            len = 2;
        } else {                                             // <Feature Abort> [0x8C, unrecognized]
            frame[0] = (uint8_t)((s_cec_la << 4) | s_cec_reply_to);
            frame[1] = 0x00;
            frame[2] = 0x8C;
            frame[3] = 0x00;
            len = 4;
        }
        if (adv7511_cec_tx(frame, len)) s_cec_reply_op = 0;
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
    if (s_cec_rx_pending && (HAL_GetTick() - s_cec_rx_since) >= CEC_RELAY_HOLD_MS) {
        debug_log("[CEC] rx dropped\r\n");
        adv7511_cec_rx_ack();
    }
    if (!s_cec_rx_pending) {
        if (adv7511_cec_read_register(0x49) & 0x01) {
            uint8_t len = adv7511_cec_read_register(0x25) & 0x1F;
            if (len > 0 && len <= ADV7511_CEC_MAX_FRAME) {
                // Read the whole frame once, here, rather than lazily per-byte from
                // adv7511_cec_rx_read() -- the SMBus relay's own Rx opcode fetches one byte
                // per I2C transaction from the console side, and re-reading all of 0x15..
                // over the CEC map on every single one of those would be up to
                // ADV7511_CEC_MAX_FRAME times the I2C-CEC traffic for no benefit (the
                // buffer's contents don't change while unacked).
                for (uint8_t i = 0; i < len; i++) {
                    s_cec_rx_buf[i] = adv7511_cec_read_register((uint8_t)(0x15 + i));
                }
                s_cec_rx_len = len;
                __DMB();   // the frame is in place before the ISR can see it pending
                s_cec_rx_pending = true;
                // Requests to our claimed address that X-HD answers itself (One Touch Play
                // claimed one; without it the chip takes no directed frames): name and
                // physical address -- what TVs ask before listing a device. Taken off the
                // relay; everything else stays for the BIOS.
                // <Give Device Vendor ID> gets <Feature Abort>: CEC's vendor ID is an IEEE
                // OUI X-HD doesn't have. TVs ask it first; unanswered, it sat in the relay
                // and blocked every later frame (the TV's <Give OSD Name> never arrived).
                const uint8_t op = (len >= 2) ? s_cec_rx_buf[1] : 0;
                cec_track_route(op, len);
                const bool ours = s_cec_la && (s_cec_rx_buf[0] & 0x0F) == s_cec_la &&
                                  (op == 0x46 || op == 0x83 || op == 0x8C);
                cec_log("rx", s_cec_rx_buf, len, ours ? " ans" : " fwd");
                s_cec_rx_since = HAL_GetTick();
                if (ours) {
                    s_cec_reply_op = op;
                    s_cec_reply_to = s_cec_rx_buf[0] >> 4;
                    adv7511_cec_rx_ack();
                }
            } else {
                // Hardware reported a frame with an impossible length -- discard it
                // (pulse the ready-clear bit the same as a normal ack) rather than latch a
                // pending frame the relay could never correctly size for the BIOS.
                cec_rx_clear();
            }
        }
    }
}

// One Touch Play -- the only CEC protocol X-HD does itself (everything else is the SMBus relay):
// claim a Playback logical address (poll 4, 8, 11; the first nobody ACKs is free), send
// <Image View On> to the TV (wakes it), then -- once the woken TV raised HPD and its EDID gave
// our physical address -- broadcast <Active Source> (it switches to this input), then send the
// TV our <Set OSD Name> (the SPD product; device lists show it). Starts at boot
// whatever HPD says: a TV in standby drops HPD but still listens on CEC, so waiting for HPD
// never woke it. One step per completed send, so it never blocks; a result-less status (a
// re-init, arbitration lost) resends the same step. Arbitration lost waits a second first, is
// logged once, and gives up for the boot after 5 resends in a row: a CEC line held low (no
// CEC sink pulling it up) loses every send at once, and resending each pass flooded the UART.
// Runs to completion once per MCU boot.
static const uint8_t k_playback_la[3] = { 4, 8, 11 };
// s_otp_step: 0-2 polling k_playback_la[step], 3 Image View On, 4 Active Source, 5 Set OSD Name, 6 done
static uint8_t s_otp_step = 0;
static bool s_otp_sent = false;
static uint32_t s_otp_lost_at = 0;
static uint8_t s_otp_lost = 0;   // arbitration losses in a row
void adv7511_cec_one_touch_step(uint16_t phys_addr) {
    if (s_otp_step >= 6) return;
    // The chip's own setup (and its CEC init) waits for HPD; CEC doesn't need it, and its
    // registers (0xE1/0xE2, the CEC Map) sit outside what an HPD drop resets.
    if (!s_cec_inited) adv7511_cec_init();
    if (s_otp_sent) {
        const uint8_t st = s_cec_tx_status;
        if (st & ADV7511_CEC_TX_BUSY) return;
        s_otp_sent = false;
        if (st & ADV7511_CEC_TX_ARB_LOST) {
            if (++s_otp_lost > 5) {
                debug_log("[CEC] arb lost, One Touch Play off\r\n");
                s_otp_step = 6;
                s_cec_tx_quiet = false;
                return;
            }
            if (!s_cec_tx_quiet) debug_log("[CEC] arb lost, retrying 1/s\r\n");
            s_cec_tx_quiet = true;
            s_otp_lost_at = HAL_GetTick();
        }
        if (st & (ADV7511_CEC_TX_OK | ADV7511_CEC_TX_TIMEOUT)) {
            s_cec_tx_quiet = false;
            s_otp_lost = 0;
            if (s_otp_step >= 3) {
                s_otp_step++;
            } else if (st & ADV7511_CEC_TX_TIMEOUT) {         // no ACK: the address is free
                s_cec_la = k_playback_la[s_otp_step];
                adv7511_cec_update_register(0x4C, 0x0F, s_cec_la);
                s_otp_step = 3;
            } else if (++s_otp_step == 3) {                   // all three taken: give up
                s_otp_step = 6;
            }
            if (s_otp_step >= 6) return;
        }
    }
    if (s_cec_tx_quiet && (HAL_GetTick() - s_otp_lost_at) < 1000u) return;
    uint8_t frame[ADV7511_CEC_MAX_FRAME];
    uint8_t len;
    if (s_otp_step < 3) {
        frame[0] = (uint8_t)(k_playback_la[s_otp_step] * 0x11); // polling: from and to the candidate
        len = 1;
    } else if (s_otp_step == 3) {
        frame[0] = (uint8_t)(s_cec_la << 4);                // to the TV (0)
        frame[1] = 0x04;                                     // <Image View On>
        len = 2;
    } else if (s_otp_step == 5) {
        len = cec_osd_name_frame(frame, s_cec_la, 0);        // to the TV
    } else {
        if (phys_addr == 0xFFFF) return;                     // no EDID yet: TV still waking
        len = cec_pa_frame(frame, 0x82, phys_addr);          // <Active Source>
        s_cec_active = true;
    }
    s_otp_sent = adv7511_cec_tx(frame, len);
}

// <Standby> to the TV (console shutting down), sent by adv7511_cec_poll() once Tx is free.
// Takes the reply slot: nothing queued there matters with the console going off. From our
// claimed address, or 15 (unregistered) when One Touch Play never claimed one. Skipped when
// the bus said the TV shows another input (cec_track_route()): don't turn off what's in use.
void adv7511_cec_standby(void) {
    if (!s_cec_active) {
        debug_log("[CEC] standby skipped: not active source\r\n");
        return;
    }
    s_cec_reply_op = 0x36;
}

// ISR-safe (the SMBus relay): queues one frame for adv7511_cec_poll() to send. false, like
// adv7511_cec_tx(), for a bad length or while a send is in flight or already queued.
bool adv7511_cec_tx_queue(const uint8_t *frame, uint8_t len) {
    if (len == 0 || len > ADV7511_CEC_MAX_FRAME) return false;
    if (s_cec_tx_req_len || (s_cec_tx_status & ADV7511_CEC_TX_BUSY)) return false;
    memcpy(s_cec_tx_req, frame, len);
    s_cec_tx_req_len = len;
    return true;
}

// ISR-safe: the relay is done with the received frame; adv7511_cec_poll() frees the receiver.
// The frame reads as gone at once, so the BIOS can't read it twice.
void adv7511_cec_rx_release(void) {
    if (s_cec_rx_pending) s_cec_rx_release = true;
}

// A queued relay send reads as in flight until it has gone out and finished.
uint8_t adv7511_cec_tx_status(void) {
    return s_cec_tx_req_len ? ADV7511_CEC_TX_BUSY : s_cec_tx_status;
}

bool adv7511_cec_rx_pending(uint8_t *len) {
    if (!s_cec_rx_pending || s_cec_rx_release) return false;
    if (len) *len = s_cec_rx_len;
    return true;
}

void adv7511_cec_rx_read(uint8_t *out) {
    if (!s_cec_rx_pending || s_cec_rx_release || !out) return;
    for (uint8_t i = 0; i < s_cec_rx_len; i++) {
        out[i] = s_cec_rx_buf[i];
    }
}

void adv7511_cec_rx_ack(void) {
    s_cec_rx_release = false;
    if (!s_cec_rx_pending) return;
    cec_rx_clear();
    s_cec_rx_pending = false;
    s_cec_rx_len = 0;
}
#endif
