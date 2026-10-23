#ifndef __SMBUS_PROBE_H__
#define __SMBUS_PROBE_H__

#include <stdbool.h>
#include <stdint.h>
#include "../shared/types.h"

// Passive real-Xbox-encoder-address bus probe -- see smbus_probe.c for the full design
// rationale. Watches raw SCL/SDA transitions on I2C2's own pins for a START condition
// followed by an address byte matching one of the three known real video-encoder SMBus
// addresses (Focus 0xD4, Conexant 0x8A, Xcalibur 0xE0), bounded to a short window (armed
// on demand, self-disarms on match) rather than running continuously.
void smbus_arm_encoder_probe(void);
void smbus_disarm_encoder_probe(void);

// Passive Xbox factory EEPROM field probe -- shares the encoder probe's EXTI taps (see
// smbus_probe.c's probe_sync_imr()). Captures VideoStandard (EEPROM offset 0x58-0x5B) and
// VideoSettings (0x94-0x97) off the console's own boot-time EEPROM readout. The field
// capture is a no-op once both are set, but the state machine stays armed all session --
// it's shared with the encoder register decode (IN_MODE, WSS, widths, Xcalibur raster),
// which must keep tracking. Armed by smbus_probe_early_init().
// Getters return false until a valid value has actually been seen.
// EXTI setup + arm for the passive PB10/PB11 probe. Call BEFORE init_adv() in main() so
// the probe is live for the console's earliest boot-time encoder config -- some BIOSes
// write the Conexant IN_MODE pixel-format reg only once, early in boot. The only arm at
// boot. Needs GPIOB+SYSCFG clocks and EXTI4_15_IRQn enabled (main() does the latter).
void smbus_probe_early_init(void);
bool xbox_eeprom_get_video_standard(uint32_t *out); // raw 4-byte LE field
bool xbox_eeprom_get_video_settings(uint32_t *out); // raw 4-byte LE field
// Passively-decoded encoder digital-input pixel format (Conexant IN_MODE[3:0] off the same
// SMBus tap; probe stays armed all session). false = unknown (not seen / Focus/Xcalibur
// not decoded). On true, *is_rgb = GPU is driving RGB (not YCrCb); *in_mode_raw (optional,
// may be NULL) = raw IN_MODE nibble.
bool xbox_encoder_input_is_rgb(bool *is_rgb, uint8_t *in_mode_raw);
// True (self-clearing) when that verdict changed since last consumed -- the supervisor reapplies.
bool smbus_encfmt_take_dirty(void);
// Passively-sniffed Focus FS454 IHW (Input Horizontal Width, datasheet reg 0x04/0x05) -- a
// genuine GPU-programmed active-width count, independent of the ADV7511 pixel clock (which
// is not on its own a reliable width signal for Focus SD -- see smbus_probe.c). Always known:
// it starts at the chip's power-on 720, so callers check the encoder first (Conexant/
// Xcalibur have no such register). The latest raw pixel count.
uint16_t xbox_focus_get_ihw(void);
// Bumped on every IHW (0x04/0x05) or IHO (0x00) write this boot, regardless of whether the value
// changed -- lets a caller detect "this was genuinely rewritten" even when two different
// rasters happen to share the same raw IHW reading (a plain value-equality check can't).
// Compare against a previously-saved value; never read for its own absolute magnitude.
uint8_t xbox_focus_get_ihw_epoch(void);
// Passively-sniffed Focus IHO low byte (Input Horizontal Offset, datasheet reg 0x00) --
// distinguishes 1.1:1 PAR content from standard content within the same IHW width reading
// (see smbus_probe.c's FS_REG_IHO_LO comment for the confirmed reference values). false
// until register 0x00 has been seen (Conexant/Xcalibur: no such register, always false). On
// true, *out = latest raw byte.
bool xbox_focus_get_iho(uint8_t *out);
// One dispatcher over both encoders' interlace/progressive signal, same precedent as
// xbox_encoder_get_wss_widescreen() below. Focus: VID_CNTL0 bit7 / INT_PROG (datasheet reg
// 0x92, same register the RGB/YCrCb decode above already reads) -- per the Focus Software/
// Firmware Reference, "Interlaced/Progressive. When set=1, input image is interlaced."
// Conexant: RASTER_SEL/HDTV_EN (reg 0x2E, see CX_REG_RASTER_SEL in smbus_probe.c) --
// true=interlaced, same convention. Xcalibur: no such register on either path, always false.
// false until the relevant register has been seen at least once this boot. On true, *out =
// true if the input is genuinely interlaced.
bool xbox_encoder_get_int_prog(const xbox_encoder enc, bool *out);
// Bumped on every real register write (0x92 for Focus, 0x2E for Conexant) this boot -- same
// "genuinely rewritten" vs "value happens to match" distinction as xbox_focus_get_ihw_epoch()
// above. 0 for Xcalibur, or for an encoder that hasn't had a write yet this boot.
uint8_t xbox_encoder_get_int_prog_epoch(const xbox_encoder enc);
// Passively-sniffed live WSS aspect broadcast to TVs -- Focus's WSS_DATAF1 (reg 0x83) or
// Conexant's WSSDAT (reg 0x60, region-dependent bit encoding -- see CX_REG_WSS_DATA_B0's own
// comment in smbus_probe.c), unlike the EEPROM Widescreen bit which is captured once at boot.
// false until the relevant byte has been seen this boot, or its value isn't a known encoding
// for that encoder. Xcalibur: no such register, always false. On true, *out = true (16:9) or
// false (4:3).
bool xbox_encoder_get_wss_widescreen(const xbox_encoder enc, bool *out);
// Passively-sniffed Conexant CX25870/871 H_ACTIVE (datasheet reg 0x78/0x86) -- genuine
// GPU/dashboard-sent geometry, same mechanism as the Focus IHW above. Written fresh on every
// genuine SD mode entry -- see cx_hactive_classify_instant() in xbox_video_standalone.c for
// how its value resolves to a width verdict. false until 0x78 has been written since boot
// or the last switch to a progressive raster (the high bits are pre-seeded).
bool xbox_cx_get_hactive(uint16_t *out);
// Bumped on every real 0x78 (H_ACTIVE_LO) write this boot: tells "rewritten" from "happens
// to match" (two modes can share a value).
uint8_t xbox_cx_get_hactive_epoch(void);

// The Xcalibur's mode index, derived from its own raster registers (any BIOS); 0 = none yet.
// Register-identical pairs read as the 720-wide one: 0x0F -> 0x02, 0x10 -> 0x04, 0x11 -> 0x08.
uint8_t xbox_xcal_mode_index(void);
// Caller buffer size smbus_enc_trace() requires/fills per call (each of addr/reg/val).
#define ENC_TRACE_PAGE 64
// Bring-up trace: encoder-register writes (all 3 encoder addresses), oldest first, starting
// `skip` entries into the available ring (0 = from the very oldest available) so a caller
// with a small ENC_TRACE_PAGE-sized buffer can page through a ring bigger than that buffer
// across several calls -- see debug.c's "encdump" handler. Returns count written into
// addr/reg/val (each >= ENC_TRACE_PAGE) for this page; *lifetime = saturating total ever
// seen (not page-relative).
uint8_t smbus_enc_trace(uint8_t *addr, uint8_t *reg, uint8_t *val, uint16_t skip, uint16_t *lifetime);
// Traced writes overwritten before the main loop decoded them (saturating) -- nonzero means
// some decoded encoder state may have missed a write.
uint16_t smbus_enc_trace_lost(void);
// SMC writes seen since the last call, each reported once: the audio clamp (a reset or
// shutdown has started) and a shutdown (power button or software; never a reboot).
#define SMC_EV_AUDIO_CLAMP 0x01
#define SMC_EV_SHUTDOWN    0x02
uint8_t smbus_smc_events_take(void);
// Decode traced encoder writes (main loop only). The getters above call it themselves.
void smbus_probe_drain(void);
// Manual override -- inject an encoder address so smbus_check_probed_encoder() self-latches
// it. `addr` = ENCODER_XCALIBUR / _CONEXANT / _FOCUS.
void smbus_force_encoder(uint8_t addr);
uint8_t smbus_get_probed_encoder_addr(void);
bool smbus_probe_activity_seen(void);
bool smbus_host_addressed_us(void);
void smbus_probe_scl_edge(void);
void smbus_probe_sda_edge(void);
void smbus_check_probed_encoder(xbox_encoder *xb_encoder, bool *encoder_mismatch, bool *needs_reapply);
// Switch to `enc` (bus setup now; saved to page 63, *encoder_mismatch set, if it differs from
// the saved one) -- the probe's and the BIOS packet's shared self-latch.
void smbus_latch_encoder(xbox_encoder enc, xbox_encoder *xb_encoder, bool *encoder_mismatch);

// smbus_i2c.c's HAL_I2C_AddrCallback feeds these -- being addressed at our own slave
// address is itself proof of SMBus activity/a host present, tracked even when the passive
// edge probe isn't armed. See smbus_probe.c's own comment on these two setters.
void smbus_note_probe_activity(void);
void smbus_note_host_addressed(void);

#endif // __SMBUS_PROBE_H__
