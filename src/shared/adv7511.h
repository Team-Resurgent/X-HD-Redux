// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT

#ifndef __ADV7511_H__
#define __ADV7511_H__

#include "stm32.h"
#include "stdbool.h"
#include "types.h"

#define ADV7511_VIC_CHANGED         0x80
#define ADV7511_VIC_CHANGED_CLEAR   0x7F
// Bits 0-4 of the mailbox: the VIC value (max CEA VIC used in this codebase is 22, fits in
// 5 bits). Bit 5 carries the resolved AVI 16:9 aspect alongside it, bit 6 marks a plain
// arbiter decision that may take the in-place aspect path (relock/force-reapply entries
// leave it clear so they always gate) -- both only meaningful when ADV7511_VIC_CHANGED is
// set; see xbox_video_standalone.c's stand_alone_loop() pack/unpack.
#define ADV7511_VIC_MASK            0x1F
#define ADV7511_VIC_WIDESCREEN      0x20
#define ADV7511_VIC_INPLACE_OK      0x40

/* Hardware defined default addresses for I2C register maps */
#define ADV7511_MAIN_I2C_ADDR           0x72 //0x72>>1
// Packet Memory Map I2C address -- programmable via Main Map 0x45, left at its
// power-up default (0x70). Holds the SPD/MPEG/ACP/ISRC/Spare secondary packets.
#define ADV7511_PACKET_MEM_I2C_ADDR     0x70
// CEC Map I2C address -- programmable via Main Map 0xE1, power-up default 0x78 (Programming
// Guide Table, "CEC Map Address" 0xE1 = 0b01111000). Written explicitly below rather than
// assumed, same defensive style as the packet memory map.
#define ADV7511_CEC_I2C_ADDR            0x78

static const uint8_t CscYuvToRgb709[] = {
    0xE7, 0x34, 0x04, 0xAD, 0x00, 0x00, 0x1C, 0x1B,
    0x1D, 0xDC, 0x04, 0xAD, 0x1F, 0x24, 0x01, 0x35,
    0x00, 0x00, 0x04, 0xAD, 0x08, 0x7C, 0x1B, 0x77};

// Table 32: HDTV YCbCr (Limited Range) to RGB (Limited Range)
static const uint8_t CscYuvToRgb709Limited[] = {
    0xAC, 0x53, 0x08, 0x00, 0x00, 0x00, 0x19, 0xD6,
    0x1C, 0x56, 0x08, 0x00, 0x1E, 0x88, 0x02, 0x91,
    0x1F, 0xFF, 0x08, 0x00, 0x0E, 0x85, 0x18, 0xBE};

// Table 35: SDTV YCbCr (Limited Range) to RGB (Full Range)
static const uint8_t CscYuvToRgb601[] = {
    0xE6, 0x69, 0x04, 0xAC, 0x00, 0x00, 0x1C, 0x81,
    0x1C, 0xBC, 0x04, 0xAD, 0x1E, 0x6E, 0x02, 0x20,
    0x1F, 0xFE, 0x04, 0xAD, 0x08, 0x1A, 0x1B, 0xA9};

// Table 34: SDTV YCbCr (Limited Range) to RGB (Limited Range)
static const uint8_t CscYuvToRgb601Limited[] = {
    0xAA, 0xF8, 0x08, 0x00, 0x00, 0x00, 0x1A, 0x84,
    0x1A, 0x6A, 0x08, 0x00, 0x1D, 0x50, 0x04, 0x23,
    0x1F, 0xFC, 0x08, 0x00, 0x0D, 0xDE, 0x19, 0x13};

typedef struct
{
    volatile uint8_t interrupt; // set in the EXTI ISR (interrupt.c), polled from the main loop
    uint8_t vic;
} adv7511;

void adv7511_power_up(void);
void adv7511_ensure_powered_up(void);
void adv7511_update_register(const uint8_t address, const uint8_t mask, uint8_t new_value);
uint8_t adv7511_read_register(const uint8_t address);
void adv7511_write_register(const uint8_t address, uint8_t value);
void adv7511_struct_init(adv7511 *encoder);

// Services the interrupt flag from the main loop.
void adv_handle_interrupts(adv7511 *encoder);

#ifdef BUILD_APPLICATION
// CEC: a relay for the console's own BIOS/homebrew CEC stack (raw frames in and out through
// the SMBus opcodes in smbus_i2c.c), plus the little X-HD does itself -- One Touch Play at boot
// (it claims a playback logical address), automatic answers to <Give OSD Name>, <Give
// Physical Address> and <Give Device Vendor ID> on that address, active-source tracking and
// <Standby> at shutdown. Application only.
#define ADV7511_CEC_MAX_FRAME 16 // header + up to 15 data bytes (CEC Map's own hardware limit)
#define ADV7511_CEC_TX_BUSY      0x01 // a send is currently in flight
#define ADV7511_CEC_TX_OK        0x02 // last send acked (valid until the next adv7511_cec_tx())
#define ADV7511_CEC_TX_ARB_LOST  0x04
#define ADV7511_CEC_TX_TIMEOUT   0x08
// One-time power-up + config -- call once from adv7511_reinit_chip() alongside the other
// "fixed registers" bring-up, so CEC survives an HPD-triggered chip reset the same way.
void adv7511_cec_init(void);
// Writes `len` bytes (frame[0] is the CEC header byte) and starts the send -- main loop only
// (I2C1). false if len is out of [1,ADV7511_CEC_MAX_FRAME] or a send is still in flight.
// The SMBus relay uses adv7511_cec_tx_queue() instead.
bool adv7511_cec_tx(const uint8_t *frame, uint8_t len);
// One Touch Play, one non-blocking step per call (see adv7511.c); phys_addr from the EDID.
void adv7511_cec_one_touch_step(uint16_t phys_addr);
// Call once per main-loop iteration: sends what is queued, services Tx completion, and
// buffers a newly-arrived Rx frame if none is pending (a newer frame waits in hardware). A
// frame the relay doesn't collect within CEC_RELAY_HOLD_MS (1 s) is dropped, so a console
// with no CEC stack can't block reception.
void adv7511_cec_poll(void);
void adv7511_cec_standby(void);
uint8_t adv7511_cec_tx_status(void);
// true + *len (1..ADV7511_CEC_MAX_FRAME) while a received frame is buffered and unread.
bool adv7511_cec_rx_pending(uint8_t *len);
// Copies the pending frame into `out` (caller-sized to at least the length from
// adv7511_cec_rx_pending()). Does not itself clear the pending frame -- the relay releases
// it with adv7511_cec_rx_release().
void adv7511_cec_rx_read(uint8_t *out);
// Frees the hardware Rx buffer for the next frame, whether or not this one was read (main
// loop; the relay's ISR-side form is adv7511_cec_rx_release()).
void adv7511_cec_rx_ack(void);
// The SMBus relay's ISR-safe forms (the ISR must not touch I2C1): queue a send / release the
// received frame; adv7511_cec_poll() does the register work.
bool adv7511_cec_tx_queue(const uint8_t *frame, uint8_t len);
void adv7511_cec_rx_release(void);
#endif

void adv7511_disable_csc();
void adv7511_apply_csc(const uint8_t * const coefficients);

// Input video/clock gating (0xD6[0]) + TMDS output power. adv7511_disable/enable_video()
// gate the *input* clock/data path -- despite the name they are not output blanking; see
// adv7511.c. Cycling the input gate also forces a stuck clock-recovery PLL to re-lock.
void adv7511_disable_video();
void adv7511_enable_video();
void adv7511_power_down_tmds();
void adv7511_power_up_tmds();
// Ends the soft TMDS clock turn-on started by adv7511_power_up_tmds().
void adv7511_tmds_soft_clock_done(void);
// True while any TMDS data channel driver (0xA1[5:3]) is powered down.
bool adv7511_tmds_powered_down(void);

// Audio mute control
void adv7511_mute_audio();
void adv7511_unmute_audio();
uint32_t adv7511_get_pixel_clock_hz(void);
// CTS-derived pixel clock reads a clean 0 for ~500ms after any clock disruption, then
// snaps to the exact value (it does NOT walk). Feed adv7511_pixel_clock_note() the
// per-iteration reading; gate width/PAL classification on adv7511_pixel_clock_stable() so
// they skip the 0 window (and half-clock garbage during self-latch). reset_stability()
// on a genuine lock loss.
void adv7511_pixel_clock_note(uint32_t pclk_hz);
bool adv7511_pixel_clock_stable(void);
// Last reading that held steady for PCLK_REPORT_WINDOW_MS -- what to REPORT (CLI, telemetry,
// sweeps). 0 = no settled measurement since the last loss. Ignores lone outliers.
uint32_t adv7511_pixel_clock_settled_hz(void);
// The latest raw reading fed to adv7511_pixel_clock_note() -- non-zero means the chip's
// audio clock regeneration (CTS) is running.
uint32_t adv7511_pixel_clock_last_hz(void);
void adv7511_pixel_clock_reset_stability(void);

#endif // __ADV7511_H__
