// Passive SMBus bus probe: the real-Xbox-encoder-address probe and the factory EEPROM/
// encoder-format field probe. Electrically and functionally independent of the I2C2
// hardware slave protocol in smbus_i2c.c: it watches raw SCL/SDA transitions on PB10/PB11
// via EXTI (bit-banged framing, not the I2C2 peripheral), and shares no state with the
// slave command switch except through the public getters/setters in smbus_probe.h.
// smbus_i2c.c reaches it only through HAL_I2C_AddrCallback's smbus_note_probe_activity()/
// smbus_note_host_addressed(); main() arms it (smbus_probe_early_init()).
#include "smbus_probe.h"
#include "smbus_i2c.h"
#include "stm32.h"
#include "../shared/debug.h"
#include "../shared/adv7511_xbox.h"
#include "../shared/user_config.h"

// --- Passive real-Xbox-encoder-address bus probe ---
// Watches raw SCL/SDA transitions on I2C2's own pins (PB10/PB11 -- the same physical pins
// the I2C2 peripheral already uses for its own slave connection) for a START condition
// followed by an address byte matching one of the three real Xbox video-encoder SMBus
// addresses (xboxdevwiki.net/SMBus, cross-checked against the CX25870/871 datasheet's own
// "alternative device ID 0x8A"): Focus FS454 0xD4, Conexant CX25871 0x8A, Xcalibur 0xE0.
// This reveals which encoder chip is physically present by observing the real Xbox
// southbridge's own traffic to it, independent of BIOS vs standalone mode and independent
// of trusting a BIOS-reported field. Bounded/armed rather than always-on: the encoder is a
// fixed hardware property that never changes, so this only needs to run once, in a short
// window, rather than compete for interrupt time with the SMBus slave-command handling for
// the device's entire runtime.
static volatile uint8_t probe_bitcount = 0xFF; // 0xFF = idle; 0..8 = shifting; 9 = awaiting ACK bit
static volatile uint8_t probe_shiftreg = 0;
static volatile uint8_t probe_pending_addr = 0; // encoder addr byte seen, not yet ACK-confirmed
static volatile uint8_t probe_found_addr = 0;  // 0 = none found yet
static volatile bool probe_activity_seen = false; // any SCL/SDA edge at all, regardless of match

// Two independent passive listeners (this one and the EEPROM field probe below) share the
// same physical EXTI lines (PB10/PB11) and therefore the same EXTI->IMR mask bits -- there's
// only one interrupt-enable per pin, not one per listener. Each listener tracks its own
// "do I still need edges" flag; the mask is only actually cleared once BOTH say no, so one
// listener resolving early (e.g. the encoder probe, which self-disarms the instant it
// matches) can't silently cut off the other's window. See probe_sync_imr() below -- every
// arm/disarm goes through it instead of touching IMR directly.
static volatile bool s_encoder_probe_armed = false;
static volatile bool s_eeprom_probe_armed = false;

static void probe_sync_imr(void)
{
    if (s_encoder_probe_armed || s_eeprom_probe_armed) {
        EXTI->IMR |= (1u << 10) | (1u << 11);
    } else {
        EXTI->IMR &= ~((1u << 10) | (1u << 11));
    }
}
// Set the first time a master addresses us at 0x69 (nothing else on the Xbox SMBus uses it):
// a BIOS/dashboard with X-HD SMBus support is present. The supervisor uses it to choose how
// long the encoder probe stays armed.
static volatile bool s_host_addressed_us = false;

void smbus_arm_encoder_probe(void)
{
    probe_bitcount = 0xFF;
    probe_pending_addr = 0;
    probe_found_addr = 0;
    EXTI->PR = (1u << 10) | (1u << 11); // clear any stale pending bits first
    s_encoder_probe_armed = true;
    probe_sync_imr();
}

void smbus_disarm_encoder_probe(void)
{
    s_encoder_probe_armed = false;
    probe_sync_imr();
}

uint8_t smbus_get_probed_encoder_addr(void)
{
    return probe_found_addr;
}

// Tells a dead tap (no SDA edges: wire off, SDA/SCL swapped) from a live bus that just hasn't
// shown a matching address yet (update_diagnostic_leds()). SDA only: SCL toggles for the
// console's own encoder traffic whether or not our SDA tap is connected.
bool smbus_probe_activity_seen(void)
{
    return probe_activity_seen;
}

// True once a master has addressed X-HD at 0x69 -- a BIOS/dashboard with SMBus support is
// present (and will supply the encoder ID byte in its mode packets). False = no such host,
// so the passive encoder probe is the only ID path and can stay armed much longer.
bool smbus_host_addressed_us(void)
{
    return s_host_addressed_us;
}

// Applies a probed encoder through the same self-latch a BIOS packet uses (bios_loop()) --
// the console's own bus traffic is ground truth, and this works without any BIOS packet.
// Every main-loop pass; cheap once applied.
// needs_reapply: a fresh board starts with an unknown encoder, and a mode can go out before
// the probe resolves -- with the wrong encoder's timing and DDR bus order -- so it is
// reapplied once the real encoder is known.
void smbus_check_probed_encoder(xbox_encoder *xb_encoder, bool *encoder_mismatch, bool *needs_reapply)
{
    uint8_t probed = probe_found_addr;
    if (probed != 0 && *xb_encoder != (xbox_encoder)probed) {
        smbus_latch_encoder((xbox_encoder)probed, xb_encoder, encoder_mismatch);
        *needs_reapply = true;
        debug_log("[ENCODER PROBE] confirmed %s\r\n", get_xbox_encoder_name(probed));
    }
}

// The self-latch, for the probe and BIOS packets alike: the ADV7511's bus setup for `enc`,
// and -- when page 63 names another encoder (normally the first-ever boot, still on the factory
// default) -- a save, flagged for the mismatch LEDs. One-time: later boots start from it.
void smbus_latch_encoder(xbox_encoder enc, xbox_encoder *xb_encoder, bool *encoder_mismatch)
{
    *xb_encoder = enc;
    init_adv_encoder_specific(enc);
    if (user_config_get()->active_encoder != user_config_get_encoder_index(enc)) {
        user_config_set_active_encoder(enc);
        *encoder_mismatch = true;
    }
}

// Set the encoder as if the probe had captured it: smbus_check_probed_encoder() then
// self-latches and reapplies on the next pass. For the `enc` CLI command and the RECOVERY-pin
// cycle. addr: ENCODER_XCALIBUR/_CONEXANT/_FOCUS (0xE0/0x8A/0xD4).
void smbus_force_encoder(uint8_t addr)
{
    probe_found_addr = addr;
    smbus_disarm_encoder_probe();  // we've resolved it; get the EXTI off the bus
}

// --- Passive Xbox factory EEPROM field probe ---
// Shares the encoder probe's EXTI taps (see probe_sync_imr() above) but decodes a full
// multi-byte read transaction instead of a single address byte -- needed to passively
// capture two fields from the console's own EEPROM readout (which it already performs once
// at boot, unprompted, at 7-bit address 0x54 / 8-bit 0xA8 write, 0xA9 read):
//   VideoStandard, offset 0x58-0x5B (4 bytes LE) -- NTSC-M/NTSC-J/PAL-I/PAL-M
//   VideoSettings,  offset 0x94-0x97 (4 bytes LE) -- Widescreen/Letterbox/50Hz/60Hz/etc bits
// Layout verified byte-for-byte against a real bus capture and xboxdevwiki.net/EEPROM --
// serial number, timezone name strings, and zone bias all decoded correctly at their
// documented offsets before these two were trusted.
//
// State machine: a genuine repeated START ("Start repeat", not a STOP+START)
// and a fresh START are electrically identical (SDA falls while SCL is
// high either way), so both are handled by the same transition into EEPROM_ADDR without
// needing to special-case "repeated". Any address byte that isn't 0xA8/0xA9 drops straight
// back to idle -- a decode slip can never wedge this into misreading unrelated traffic.
// Byte position is tracked as an absolute EEPROM offset (last-written offset + bytes seen
// so far in this read), not a fixed expected read length, so it doesn't matter how many
// bytes-per-transaction the real Xbox EEPROM-read routine happens to use.
typedef enum {
    EEPROM_ST_IDLE = 0,
    EEPROM_ST_ADDR,       // shifting in the address+R/W byte
    EEPROM_ST_ADDR_ACK,   // skip the ack/nack clock after it
    EEPROM_ST_OFFSET,     // shifting in the register-select (offset) byte -- write side
    EEPROM_ST_OFFSET_ACK, // skip ack; an encoder write then advances to EEPROM_ST_DATA
    EEPROM_ST_DATA,       // shifting in a data byte -- EEPROM read side / encoder write side
    EEPROM_ST_DATA_ACK,   // skip ack/nack
    EEPROM_ST_PARK,       // EEPROM offset acked: waits for the repeated START of the read
} eeprom_probe_state_t;
// The *_ACK states are the ACK slot: 8 bits clocked, the 9th clock not yet. SDA edges there
// are the ACK itself, never framing (see eeprom_probe_sda_edge()).

// The in-flight transaction's address byte: 0xA8 write / 0xA9 read = the Xbox factory
// EEPROM; 0x8A Conexant, 0xD4 Focus, 0xE0 Xcalibur encoder writes; 0x20 SMC writes. Same
// framing for all.
#define ADDR_EEPROM_W 0xA8
#define ADDR_EEPROM_R 0xA9
#define ADDR_ENC_CX   0x8A
#define ADDR_ENC_FS   0xD4
#define ADDR_ENC_XCAL 0xE0
#define ADDR_SMC      0x20
// SMC commands. AUDIO_CLAMP 1 = clamp: the console writes it on a reboot, a shutdown or an
// AV-pack change, and never releases it. A shutdown, never a reboot: the power button writes
// OS_RESUME = 0x02 (shutdown in progress) first; a software shutdown writes RESET = 0x80 last.
#define SMC_REG_RESET       0x02
#define SMC_REG_AUDIO_CLAMP 0x0B
#define SMC_REG_OS_RESUME   0x0D

// Bring-up trace: every register write the console makes to ANY of the three encoder
// addresses (and to the SMC, for its audio clamp), so an unfamiliar encoder's register writes can be read from one `encdump`
// instead of a reflash-per-register cycle. Ring, oldest evicted. Xcalibur writes 4 bytes per index
// (i2c block write) -- those land as 4 consecutive (reg,val) entries here; regroup offline.
//
// It is also the decode queue: the EXTI ISR only frames bytes and appends them here, and
// smbus_probe_drain() decodes them in the main loop. Decoding in the ISR made the byte's
// last clock slow enough that the ACK's SDA fall was serviced after the ACK clock had risen
// -- a false START that dropped every Xcalibur block write's data bytes after the first.
// 512 entries (power of two: free-running uint16_t counters index it by masking); 1.5 KB of
// RAM. A whole Xcalibur mode set fits.
#define ENC_TRACE_N 512
static volatile uint8_t s_enc_tr_addr[ENC_TRACE_N];  // 0x8A / 0xD4 / 0xE0 / 0x20
static volatile uint8_t s_enc_tr_reg[ENC_TRACE_N];
static volatile uint8_t s_enc_tr_val[ENC_TRACE_N];
static volatile uint16_t s_enc_tr_head = 0;          // entries ever appended (wraps at 65536)
static volatile uint16_t s_enc_tr_total = 0;         // lifetime count (saturating), = "tap alive"
static uint16_t s_enc_dec_tail = 0;                  // entries decoded (main loop)
static uint16_t s_enc_dec_lost = 0;                  // entries overwritten before decoding (saturating)

#define XBOX_EEPROM_VIDEO_STANDARD_OFFSET 0x58
#define XBOX_EEPROM_VIDEO_SETTINGS_OFFSET 0x94

// Conexant CX25870/871 digital input pixel format: IN_MODE[3:0]. Split across two regs
// (datasheet Table 1-2/1-3 + register map): [2:0] = reg 0xC6[2:0], [3] = reg 0x32[3].
// RGB modes: 0b0000/0b0001/0b0010/0b1000. Everything else (incl. the stock-Xbox 0b1100)
// is YCrCb. Captured passively off the same SMBus tap as the EEPROM readout.
//
// It can change during and after boot (a dashboard reconfiguring, a chainload to another
// BIOS), so this half of the probe stays armed all session, tracks the LATEST value, and flags
// the main loop to reapply so Auto colour follows it.
#define CX_REG_IN_MODE_LO 0xC6
#define CX_REG_IN_MODE_HI 0x32

// Focus FS454 digital input pixel format -- the FS454 analog of the Conexant
// IN_MODE above. Register 0x92 (VID_CNTL0), bit 2 (0x04) = "YUV matrix bypass" in
// the hobby focus.c. bit2 CLEAR => GPU driving RGB, bit2 SET => YCrCb. Feeds
// the same s_enc_fmt_* state + dirty flow as the Conexant path -- xbox_encoder_input_is_rgb()
// / the AUTO colour resolve don't care which encoder set it.
#define FS_REG_IN_MODE          0x92
#define FS_IN_MODE_MATRIX_BYP   0x04
// VID_CNTL0 bit 7 (INT_PROG) -- per the Focus Software/Firmware Reference, section 2.2.1.60
// "VID_CNTL0 - Video Control 0" (offset 92h): "Interlaced/Progressive. When set=1, input
// image is interlaced." This bit tracks the same SD family split the width classifier's
// pclk fallback estimates (progressive 480p family ~27.000MHz vs SDTV-style family
// ~29.455MHz on Focus) -- see xbox_video_standalone.c's width/family classifiers for how
// it's used as the primary source, with pclk only as a fallback before this bit has been
// seen this boot.
#define FS_IN_MODE_INT_PROG     0x80

// Focus FS454 IHW (Input Horizontal Width) -- per Focus's own Software/Firmware Reference,
// section 2.2.1.3: offset 0x04 = IHW[7:0], 0x05 = IHW[9:8] (bits 15-10 reserved), default
// 0x02D0 = 720, defined as "total number of active VGA pixels per line" (must be even). The
// GPU/dashboard writes its actual active input width here -- a source-driven width signal
// independent of the ADV7511-side pixel clock, which is not on its own a reliable way to
// distinguish 640 from 720 SD on Focus -- see xbox_video_standalone.c's width classifier.
#define FS_REG_IHW_LO 0x04
#define FS_REG_IHW_HI 0x05

// Focus FS454 IHO (Input Horizontal Offset) -- section 2.2.1.1, offset 0x00 (0x01 is the
// high byte, always observed 0x00 so only the low byte is tracked). Distinguishes 1.1:1 PAR
// content from standard content within the same IHW width reading -- IHW alone reads 1.1:1
// PAR as genuinely wide (the console clocks the narrow 640 pixels across the standard
// 720-wide analog line raster), which the width classifier alone can't correct.
// Observed values: NTSC standard-640 0xB6=182, standard-720 0x8F=143, 1.1:1-PAR 0x97=151;
// PAL standard-640 0x92=146, standard-720 0x67=103, 1.1:1-PAR 0x73=115.
#define FS_REG_IHO_LO 0x00

// Focus WSS_DATAF1 first byte (datasheet section 2.2.1.52, offset 0x83) -- the raw Wide
// Screen Signalling data Focus broadcasts to TVs per ITU-R BT.1119-2 (PAL) / EIAJ CPR-1204
// (NTSC); WSS_DATAF0 (0x86) is the same data for the other field and is always observed to
// mirror this byte exactly. The datasheet doesn't spell out the bit-level WSS encoding
// ("consult the WSS standard"); the encoding is per line standard, from the sweeps:
//  - 525-line (NTSC, PAL-60): 0x83 = 0x18 (4:3) / 0x60 (16:9), 0x85 = 0x00 / 0x01;
//  - 625-line (PAL 50 Hz): 0x83 = 0x00, and the EN 300 294 aspect group is in 0x85 (and 0x88,
//    the other field): 0x08 = `1000` 4:3 full format, 0x07 = `0111` 16:9 anamorphic. Every
//    PAL 4:3/16:9 pair on Focus; reading 0x83 alone left PAL
//    "unknown" and the EEPROM's Widescreen bit put 16:9 on every 4:3 mode.
// Unlike the EEPROM Widescreen bit (captured once at boot), the GPU rewrites these per
// mode/title, so they're a live signal -- see xbox_video_standalone.c's use in the arbiter
// snapshot. Only these observed values are treated as known; anything else reads as
// not-yet-confirmed rather than guessing a bit decode.
#define FS_REG_WSS_DATAF1_B0    0x83
#define FS_REG_WSS_DATAF1_B2    0x85
#define FS_WSS_ASPECT_4_3_BYTE  0x18u
#define FS_WSS_ASPECT_16_9_BYTE 0x60u
#define FS_WSS_PAL_4_3_BYTE     0x08u
#define FS_WSS_PAL_16_9_BYTE    0x07u

// Register 0x60 bits[3:0] (WSSDAT[4:1], CX25870.PDF Table 2-5) is the live aspect signal.
// It uses two different bit encodings depending on the console's region: ETSI EN 300 294
// (625-line/PAL, line 23) encodes 4:3-full-format as Group-1 bits `1000` and 16:9-anamorphic
// as `0111`; EIAJ CPR-1204/IEC 61880 (525-line/NTSC, lines 20+283) is a different standard
// whose Word-0 aspect bits encode 4:3-normal as `00` and 16:9-anamorphic as `01` -- same
// register, same nibble position, unrelated bit meanings. 0x60 reads 0xC8/0xC7 (nibble
// 0x8/0x7) on PAL50 4:3/16:9 and 0xC0/0xC1 (nibble 0x0/0x1) on NTSC60/PAL60 4:3/16:9.
// Register 0x64 also carries NTSC-only aspect payload (0x60 on 16:9, 0x00 on 4:3) but is
// CPR-1204 CRC/payload data, not needed once 0x60 resolves aspect for both regions.
#define CX_REG_WSS_DATA_B0      0x60
#define CX_WSS_NIBBLE_MASK      0x0Fu
#define CX_WSS_PAL_4_3_NIBBLE   0x08u   // ETSI EN 300 294 Group 1: 4:3 full format
#define CX_WSS_PAL_16_9_NIBBLE  0x07u   // ETSI EN 300 294 Group 1: 16:9 anamorphic
#define CX_WSS_NTSC_4_3_NIBBLE  0x00u   // EIAJ CPR-1204/IEC 61880: 4:3 normal
#define CX_WSS_NTSC_16_9_NIBBLE 0x01u   // EIAJ CPR-1204/IEC 61880: 16:9 anamorphic

// Conexant CX25870/871 H_ACTIVE ("Active Pixels per Input Line", datasheet section 2.0
// Internal Registers + Appendix A): reg 0x78 = H_ACTIVE[7:0], reg 0x86 bits[6:4] =
// H_ACTIVE[10:8] (0x86 is shared with V_ACTIVEO[8] at bit7 and H_CLKO[11:8] at bits[3:0] --
// only the masked bits belong to this field). A genuine GPU/dashboard-sent value, written on
// every real SD mode entry but never in HDTV/480p modes. See cx_hactive_classify_instant()
// in xbox_video_standalone.c for how the raw value resolves to a width verdict.
#define CX_REG_HACTIVE_LO 0x78
#define CX_REG_HACTIVE_HI 0x86
#define CX_REG_HACTIVE_HI_MASK 0x70u   // bits[6:4] of 0x86 = H_ACTIVE[10:8]
#define CX_REG_HACTIVE_HI_SHIFT 4

// RASTER_SEL (bits[1:0], 00=int-SD/480i) + HDTV_EN (bit7, 0=SDTV). Interlaced 480i writes
// 0x00; progressive 480p writes 0xAD (HDTV_EN=1, RASTER_SEL=01). See cx_sd_is_sdtv_family()
// in xbox_video_standalone.c for how this is used.
#define CX_REG_RASTER_SEL 0x2E

// Xcalibur raster registers (32-bit block writes, little-endian), decoded from full bus
// captures, NTSC and PAL (identical values under a stock and an X-HD-aware BIOS):
//   0x50 input raster   htotal << 16 | SDTV << 15 | vtotal   (525 / 625 / 750 / 1125 lines)
//   0x52 bits 31:24     active width / 16                    (0x28 = 640, 0x2D = 720)
//   0x54 output raster  as 0x50; PAL vtotal 0x208 = the 480-line hybrids (0x03/0x04)
//   0x56 low byte       0x8A on PAL 720-wide 0x04/0x06, 0x60 elsewhere in SD (0x12 vs 0x06)
//   0x07 low byte       SD WSS aspect, written after the raster: NTSC 0x00 4:3 / 0x02 16:9,
//                       PAL 0x10 4:3 / 0x0E 16:9; the whole register is 0 on 480p and HD.
// A mode set writes 50 51 52 54 55 56 in that order, so the mode index is derived once 0x56
// completes and never from a half-written group. 0x02/0x0F, 0x04/0x10 and 0x08/0x11 are
// register-identical (the GPU places the 640 picture inside the same 720 window), so they
// read as 0x02, 0x04 and 0x08.
#define XCAL_REG_ASPECT     0x07
#define XCAL_REG_IN_RASTER  0x50
#define XCAL_REG_WIDTH      0x52
#define XCAL_REG_OUT_RASTER 0x54
#define XCAL_REG_MODE_SEL   0x56
static uint32_t s_xcal_acc;          // block value being assembled, LSByte first
static uint8_t  s_xcal_acc_reg;      // its register
static uint8_t  s_xcal_acc_n;        // bytes in so far
static uint32_t s_xcal_in_raster, s_xcal_width, s_xcal_out_raster;  // this burst's values
static uint8_t  s_xcal_mode_index;   // derived at 0x56; 0 = none yet / unrecognised
static uint32_t s_xcal_aspect;       // latest 0x07 value
static bool     s_xcal_aspect_valid;

static volatile eeprom_probe_state_t eeprom_state = EEPROM_ST_IDLE;
static volatile uint8_t eeprom_bitcount = 0;
static volatile uint8_t eeprom_shiftreg = 0;
static volatile bool    eeprom_is_write = false;
static volatile uint8_t eeprom_addr = 0;         // address byte of the transaction in flight
static volatile uint8_t eeprom_cur_offset = 0;   // last offset selected by an 0xA8/enc write
static volatile uint8_t eeprom_byte_in_txn = 0;  // bytes seen so far in the current read/write

static volatile uint8_t s_eeprom_video_standard[4] = {0};
static volatile uint8_t s_eeprom_video_settings[4] = {0};
static volatile bool s_eeprom_video_standard_valid = false;
static volatile bool s_eeprom_video_settings_valid = false;

// Decoded encoder state, from here to s_cx_hactive_epoch: written by enc_decode() and read by
// the getters, all in the main loop (the ISR only fills the trace ring), so none of it is
// volatile.
static uint8_t s_enc_in_mode = 0;       // assembled Conexant IN_MODE[3:0]
static uint8_t s_enc_in_mode_have = 0;  // bit0 = 0xC6 seen, bit1 = 0x32 seen
static bool    s_enc_fmt_valid = false; // both IN_MODE halves seen at least once
static bool    s_enc_fmt_rgb   = false; // latest assembled IN_MODE is an RGB mode
static bool    s_enc_fmt_dirty = false; // RGB/YCrCb verdict changed -> main loop reapplies

// Latch a fresh RGB/YCrCb verdict off an encoder input-format write (Conexant IN_MODE, Focus
// 0x92); a change marks it dirty so the main loop re-applies.
static void note_enc_fmt(bool rgb_now) {
    if (!s_enc_fmt_valid || rgb_now != s_enc_fmt_rgb) {
        s_enc_fmt_rgb = rgb_now;
        s_enc_fmt_dirty = true;
    }
    s_enc_fmt_valid = true;
}

static bool    s_fs_int_prog       = false; // latest Focus VID_CNTL0 bit7 (INT_PROG)
static bool    s_fs_int_prog_valid = false;  // seen at least one 0x92 write this boot
// Bumped on every 0x92 write -- same "genuinely rewritten" vs "value happens to match"
// distinction as s_fs_ihw_epoch below.
static uint8_t s_fs_int_prog_epoch = 0;

// Focus FS454 power-on default (0x02D0), so always valid: it matches the chip across MCU
// resets without waiting for a real 0x04/0x05 write.
static uint16_t s_fs_ihw = 720;
// Bumped on every 0x04/0x05 or 0x00 write, regardless of whether the assembled value changed --
// lets a caller tell "genuinely rewritten this boot" apart from "raw value happens to
// match what was there before" (see xbox_video_standalone.c's own staleness-guard comment
// on why the two aren't the same thing: two different rasters can share the same raw IHW
// reading, and a value-equality check alone can't see a real rewrite happened).
static uint8_t s_fs_ihw_epoch = 0;

static uint8_t s_fs_iho       = 0;      // latest Focus IHO low byte (see FS_REG_IHO_LO)
static bool    s_fs_iho_valid = false;  // seen at least one 0x00 write this boot

static uint8_t s_fs_wss_b0       = 0;     // latest Focus WSS_DATAF1 first byte (0x83)
static uint8_t s_fs_wss_b2       = 0;     // latest Focus WSS_DATAF1 third byte (0x85)
static bool    s_fs_wss_valid    = false; // seen at least one 0x83 write this boot

static uint8_t s_cx_wss_b0       = 0;     // latest Conexant WSSDAT byte (0x60)
static bool    s_cx_wss_valid    = false; // seen at least one 0x60 write this boot

// Latest Conexant RASTER_SEL/HDTV_EN decode (0x2E) -- see CX_REG_RASTER_SEL's own comment
// above for the register encoding.
static bool    s_cx_interlaced       = false;
static bool    s_cx_interlaced_valid = false; // seen at least one 0x2E write this boot
// Bumped on every real 0x2E write, mirroring s_fs_int_prog_epoch -- lets
// xbox_video_standalone.c's staleness guard (sd_width_debounce_update()) tell "genuinely
// rewritten" from "value happens to match."
static uint8_t s_cx_interlaced_epoch = 0;

// 0x86[6:4] (H_ACTIVE[10:8]) reads a fixed "2" across every observed Conexant mode and isn't
// reliably rewritten, so the high half is pre-seeded (0x0200) and validity hangs on 0x78
// (H_ACTIVE[7:0]) alone: set by a 0x78 write, cleared on a switch to a progressive raster
// (which never rewrites 0x78/0x86 -- see the RASTER_SEL decode).
static uint16_t s_cx_hactive       = 0x0200;
static bool     s_cx_hactive_valid = false;
// Bumped on every real 0x78 write, mirroring s_fs_ihw_epoch: xbox_video_standalone.c's
// staleness check tells "rewritten" from "happens to match" by it.
static uint8_t  s_cx_hactive_epoch = 0;

static void smbus_arm_eeprom_probe(void)
{
    eeprom_state = EEPROM_ST_IDLE;
    eeprom_bitcount = 0;
    // Keeps any value already captured: re-arming doesn't discard it.
    s_eeprom_probe_armed = true;
    probe_sync_imr();
}

bool xbox_eeprom_get_video_standard(uint32_t *out)
{
    if (!s_eeprom_video_standard_valid) return false;
    *out = (uint32_t)s_eeprom_video_standard[0] | ((uint32_t)s_eeprom_video_standard[1] << 8) |
           ((uint32_t)s_eeprom_video_standard[2] << 16) | ((uint32_t)s_eeprom_video_standard[3] << 24);
    return true;
}

bool xbox_eeprom_get_video_settings(uint32_t *out)
{
    if (!s_eeprom_video_settings_valid) return false;
    *out = (uint32_t)s_eeprom_video_settings[0] | ((uint32_t)s_eeprom_video_settings[1] << 8) |
           ((uint32_t)s_eeprom_video_settings[2] << 16) | ((uint32_t)s_eeprom_video_settings[3] << 24);
    return true;
}

// Passively-decoded encoder digital-input pixel format -- the LATEST value the always-armed
// probe has read off the Conexant IN_MODE[3:0] writes (0xC6[2:0] + 0x32[3]) or the Focus
// 0x92 bit-2 write. Returns false until one has been seen (always the case on Xcalibur --
// the format is set NV2A-side there, not on the encoder bus).
// On true, *is_rgb = the GPU is driving RGB (vs YCrCb) on the digital bus.
// `in_mode_raw` (optional) = raw IN_MODE nibble (Conexant) / low nibble of 0x92 (Focus).
bool xbox_encoder_input_is_rgb(bool *is_rgb, uint8_t *in_mode_raw)
{
    smbus_probe_drain();
    if (!s_enc_fmt_valid) return false;
    if (is_rgb)      *is_rgb      = s_enc_fmt_rgb;
    if (in_mode_raw) *in_mode_raw = s_enc_in_mode & 0x0F;
    return true;
}

// Passively-sniffed Focus FS454 IHW (Input Horizontal Width) -- see FS_REG_IHW_LO/HI above.
// Always known: it starts at the chip's power-on 720 (see s_fs_ihw), so callers check the
// encoder first. The latest assembled raw pixel count the GPU/dashboard programmed (e.g. 640,
// or ~700 for a 720-wide raster).
uint16_t xbox_focus_get_ihw(void)
{
    smbus_probe_drain();
    return s_fs_ihw;
}

// See s_fs_ihw_epoch's own comment. Unconditionally returns the current count, valid or
// not -- 0 forever on Conexant/Xcalibur (no such register), same as any other never-written
// counter; a caller only needs to compare it against its own last-seen value, never its
// absolute magnitude.
uint8_t xbox_focus_get_ihw_epoch(void)
{
    smbus_probe_drain();
    return s_fs_ihw_epoch;
}

// Passively-sniffed Focus IHO low byte -- see FS_REG_IHO_LO above. Returns false until
// register 0x00 has been seen at least once this boot (always false on Conexant/Xcalibur --
// this register doesn't exist there). On true, *out = the latest raw byte the GPU/dashboard
// programmed.
bool xbox_focus_get_iho(uint8_t *out)
{
    smbus_probe_drain();
    if (!s_fs_iho_valid) return false;
    *out = s_fs_iho;
    return true;
}

// One dispatcher over both encoders' interlace/progressive signal, same precedent as
// xbox_encoder_get_wss_widescreen() below. Focus: passively-sniffed VID_CNTL0 bit7
// (INT_PROG, see FS_IN_MODE_INT_PROG above). Conexant: passively-sniffed RASTER_SEL/HDTV_EN
// (0x2E, see CX_REG_RASTER_SEL above). Both false until their own register has been seen at
// least once this boot; on true, *out = true if the GPU/encoder declared the input image
// interlaced.
bool xbox_encoder_get_int_prog(const xbox_encoder enc, bool *out)
{
    smbus_probe_drain();
    if (enc == ENCODER_FOCUS) {
        if (!s_fs_int_prog_valid) return false;
        *out = s_fs_int_prog;
        return true;
    }
    if (enc == ENCODER_CONEXANT) {
        if (!s_cx_interlaced_valid) return false;
        *out = s_cx_interlaced;
        return true;
    }
    return false;
}

// See s_fs_int_prog_epoch's/s_cx_interlaced_epoch's own comments. Same "compare, don't read
// absolutely" contract as xbox_focus_get_ihw_epoch() above -- 0 for any encoder that has
// never had a real register write this boot (Xcalibur always, or the other encoder before
// its first write), same as the epoch never having bumped.
uint8_t xbox_encoder_get_int_prog_epoch(const xbox_encoder enc)
{
    smbus_probe_drain();
    if (enc == ENCODER_FOCUS) return s_fs_int_prog_epoch;
    if (enc == ENCODER_CONEXANT) return s_cx_interlaced_epoch;
    return 0;
}

// Passively-sniffed live WSS aspect. Focus and Conexant each have their own register and
// bit encoding (Conexant's is region-dependent, see CX_REG_WSS_DATA_B0's own comment), so
// they're decoded separately, not through a shared latch. Returns false until the relevant
// byte has been seen at least once this boot, or its value doesn't match a known encoding
// for that encoder. On true, *out = true (16:9) or false (4:3).
bool xbox_encoder_get_wss_widescreen(const xbox_encoder enc, bool *out)
{
    smbus_probe_drain();
    if (enc == ENCODER_FOCUS) {
        if (!s_fs_wss_valid) return false;
        if (s_fs_wss_b0 == FS_WSS_ASPECT_16_9_BYTE) { *out = true;  return true; }
        if (s_fs_wss_b0 == FS_WSS_ASPECT_4_3_BYTE)  { *out = false; return true; }
        if (s_fs_wss_b0 == 0) {   // 625-line: the aspect group is in 0x85
            if (s_fs_wss_b2 == FS_WSS_PAL_16_9_BYTE) { *out = true;  return true; }
            if (s_fs_wss_b2 == FS_WSS_PAL_4_3_BYTE)  { *out = false; return true; }
        }
        return false;
    }
    if (enc == ENCODER_CONEXANT) {
        if (!s_cx_wss_valid) return false;
        uint8_t nibble = s_cx_wss_b0 & CX_WSS_NIBBLE_MASK;
        if (nibble == CX_WSS_PAL_16_9_NIBBLE || nibble == CX_WSS_NTSC_16_9_NIBBLE) {
            *out = true;
            return true;
        }
        if (nibble == CX_WSS_PAL_4_3_NIBBLE || nibble == CX_WSS_NTSC_4_3_NIBBLE) {
            *out = false;
            return true;
        }
        return false;
    }
    if (enc == ENCODER_XCALIBUR) {
        // 0x07 (see XCAL_REG_ASPECT). 480p/HD write it as 0 -- no aspect on the wire there,
        // so the EEPROM/user aspect decides, as for the other encoders' progressive modes.
        if (!s_xcal_aspect_valid || s_xcal_aspect == 0) return false;
        const uint8_t a = (uint8_t)s_xcal_aspect;
        if (a == 0x02 || a == 0x0E) { *out = true;  return true; }
        if (a == 0x00 || a == 0x10) { *out = false; return true; }
        return false;
    }
    return false;
}

// Passively-sniffed Conexant CX25870/871 H_ACTIVE -- see CX_REG_HACTIVE_LO/HI above. Returns
// false until 0x78 has been written since boot or the last progressive switch. On true,
// *out = the latest assembled raw pixel count.
bool xbox_cx_get_hactive(uint16_t *out)
{
    smbus_probe_drain();
    if (!s_cx_hactive_valid) return false;
    *out = s_cx_hactive;
    return true;
}

// See s_cx_hactive_epoch's own comment above.
uint8_t xbox_cx_get_hactive_epoch(void)
{
    smbus_probe_drain();
    return s_cx_hactive_epoch;
}


// The Xcalibur's mode index, derived from its raster registers at the end of each mode set
// (see XCAL_REG_IN_RASTER): 0 until the first one, or for a raster not in the table.
uint8_t xbox_xcal_mode_index(void)
{
    smbus_probe_drain();
    return s_xcal_mode_index;
}

static uint8_t xcal_derive_mode_index(const uint8_t mode_sel)
{
    const uint16_t vtotal = (uint16_t)(s_xcal_in_raster & 0x7FFFu);
    const bool sdtv = (s_xcal_in_raster & 0x8000u) != 0;
    const bool narrow = (s_xcal_width >> 24) == 0x28;          // 640 / 16
    switch (vtotal) {
        case 0x20D: return sdtv ? (narrow ? 0x01 : 0x02) : (narrow ? 0x07 : 0x08);
        case 0x271:
            if ((s_xcal_out_raster & 0x7FFFu) == 0x208) return narrow ? 0x03 : 0x04;
            if (narrow) return 0x05;
            return (mode_sel == 0x8A) ? 0x06 : 0x12;
        case 0x2EE: return 0x0A;
        case 0x465: return ((s_xcal_width >> 24) == 0x5A) ? 0x0C : 0x0D;   // 1440 / 1920
        default:    return 0;
    }
}

// SMC_EV_* seen since the last call (each once).
static uint8_t s_smc_events = 0;
uint8_t smbus_smc_events_take(void)
{
    smbus_probe_drain();
    const uint8_t ev = s_smc_events;
    s_smc_events = 0;
    return ev;
}

// True (once) when the encoder's input format (RGB vs YCrCb) changed since the last call; the
// supervisor then reapplies so Auto colour follows.
bool smbus_encfmt_take_dirty(void)
{
    smbus_probe_drain();
    if (!s_enc_fmt_dirty) return false;
    s_enc_fmt_dirty = false;
    return true;
}

// Bring-up trace (`encdump`): copies up to ENC_TRACE_PAGE entries, oldest first, starting
// `skip` past the oldest, so a small buffer can page through the ring. Returns the count.
// *lifetime (optional): the saturating total -- nonzero means the tap sees traffic.
uint8_t smbus_enc_trace(uint8_t *addr, uint8_t *reg, uint8_t *val, uint16_t skip, uint16_t *lifetime)
{
    if (lifetime) *lifetime = s_enc_tr_total;
    uint16_t have = (s_enc_tr_total < ENC_TRACE_N) ? s_enc_tr_total : ENC_TRACE_N;
    if (skip >= have) return 0;
    have = (uint16_t)(have - skip);
    uint8_t n = (have < ENC_TRACE_PAGE) ? (uint8_t)have : ENC_TRACE_PAGE;
    uint16_t idx = (uint16_t)(s_enc_tr_head - have);   // oldest position after skip
    for (uint8_t i = 0; i < n; i++, idx++) {
        addr[i] = s_enc_tr_addr[idx & (ENC_TRACE_N - 1)];
        reg[i]  = s_enc_tr_reg[idx & (ENC_TRACE_N - 1)];
        val[i]  = s_enc_tr_val[idx & (ENC_TRACE_N - 1)];
    }
    return n;
}

// Traced writes the decoder never saw (overwritten first; see smbus_probe_drain()).
uint16_t smbus_enc_trace_lost(void)
{
    return s_enc_dec_lost;
}

// One traced encoder write, decoded (main loop, oldest first -- see smbus_probe_drain()).
static void enc_decode(const uint8_t addr, const uint8_t reg, const uint8_t val)
{
    if (addr == ADDR_SMC) {
        if (reg == SMC_REG_AUDIO_CLAMP && val == 0x01) s_smc_events |= SMC_EV_AUDIO_CLAMP;
        if ((reg == SMC_REG_OS_RESUME && val == 0x02) || (reg == SMC_REG_RESET && val == 0x80))
            s_smc_events |= SMC_EV_SHUTDOWN;
        return;
    }
    // Decode the digital input colour space (RGB vs YCrCb) -> shared
    // s_enc_fmt_* + dirty flow. Conexant: IN_MODE[3:0] (0xC6[2:0] + 0x32[3]).
    // Focus: 0x92 bit 2. Xcalibur: not on the bus (set NV2A-side).
    bool cx_in_mode_touched = false;
    if (addr == ADDR_ENC_CX) {
        if (reg == CX_REG_IN_MODE_LO) {   // 0xC6[2:0] = IN_MODE[2:0]
            s_enc_in_mode = (uint8_t)((s_enc_in_mode & 0x08) | (val & 0x07));
            s_enc_in_mode_have |= 0x01;
            cx_in_mode_touched = true;
        } else if (reg == CX_REG_IN_MODE_HI) {  // 0x32[3] = IN_MODE[3]
            s_enc_in_mode = (uint8_t)((s_enc_in_mode & 0x07) | (val & 0x08));
            s_enc_in_mode_have |= 0x02;
            cx_in_mode_touched = true;
        } else if (reg == CX_REG_HACTIVE_LO) {   // H_ACTIVE[7:0]
            s_cx_hactive = (uint16_t)((s_cx_hactive & 0xFF00u) | val);
            s_cx_hactive_valid = true;
            s_cx_hactive_epoch++;
        } else if (reg == CX_REG_HACTIVE_HI) {   // H_ACTIVE[10:8], masked (see comment above)
            uint16_t hi = (val & CX_REG_HACTIVE_HI_MASK) >> CX_REG_HACTIVE_HI_SHIFT;
            s_cx_hactive = (uint16_t)((s_cx_hactive & 0x00FFu) | (hi << 8));
        } else if (reg == CX_REG_WSS_DATA_B0) {
            // See CX_REG_WSS_DATA_B0's own comment for the register/encoding.
            s_cx_wss_b0 = val;
            s_cx_wss_valid = true;
        } else if (reg == CX_REG_RASTER_SEL) {
            // bits[1:0] RASTER_SEL (00 = int-SD/480i), bit7 HDTV_EN (0 = SDTV).
            s_cx_interlaced = ((val & 0x03) == 0x00) && ((val & 0x80) == 0x00);
            s_cx_interlaced_valid = true;
            s_cx_interlaced_epoch++;
            // WSS (0x60) is an SD-interlaced-only standard (NTSC line 20 / PAL line
            // 23) -- Conexant never writes it in progressive 480p, so a WSS byte
            // latched during a preceding interlaced/PAL mode would otherwise be
            // trusted straight through a transition into progressive content that
            // never rewrites it. Invalidate on every confirmed progressive
            // transition; a later real interlaced mode re-latches it normally on its
            // own next write.
            if (!s_cx_interlaced) {
                s_cx_wss_valid = false;
                // Same stale-latch class as WSS above, for H_ACTIVE (0x78/0x86): the
                // console's HDTV/480p register writes never touch 0x78/0x86, so a
                // narrow value latched during a preceding interlaced mode would
                // otherwise survive into a progressive mode and be trusted as that
                // stream's own width. The next genuine interlaced entry rewrites 0x78
                // fresh and re-validates it.
                s_cx_hactive_valid = false;
            }
        }
        if (cx_in_mode_touched && s_enc_in_mode_have == 0x03) {
            uint8_t m = s_enc_in_mode & 0x0F;
            bool rgb_now = (m == 0x00 || m == 0x01 || m == 0x02 || m == 0x08);
            note_enc_fmt(rgb_now);
        }
    } else if (addr == ADDR_ENC_FS && reg == FS_REG_IN_MODE) {
        // 0x92 bit2 clear => RGB, set => YCrCb (see FS_REG_IN_MODE above).
        // Some BIOSes write 0x92 only once, so latch on the single write --
        // same "written once, games don't touch it" case as the CX path.
        s_enc_in_mode = val;   // raw 0x92 for the `eeprom` diag
        bool rgb_now = (val & FS_IN_MODE_MATRIX_BYP) == 0;
        note_enc_fmt(rgb_now);
        s_fs_int_prog = (val & FS_IN_MODE_INT_PROG) != 0;
        s_fs_int_prog_valid = true;
        s_fs_int_prog_epoch++;
        // Symmetric to the Conexant WSS invalidation above -- Focus's own WSS (0x83)
        // is the same SD-interlaced-only standard, so a stale latch from a preceding
        // interlaced mode has the identical risk of bleeding into progressive 480p
        // content that never rewrites it. WSS is only ever transmitted on interlaced
        // video standards, regardless of encoder.
        if (!s_fs_int_prog) {
            s_fs_wss_valid = false;
        }
    } else if (addr == ADDR_ENC_FS &&
               (reg == FS_REG_IHW_LO || reg == FS_REG_IHW_HI)) {
        // IHW[7:0]/[9:8] (bits 15-10 of the 0x05 byte are reserved, not masked out
        // here since every observed sample has them at 0) -- see FS_REG_IHW_LO/HI
        // above. Latest-of-each-half, same pattern as the Conexant IN_MODE assembly:
        // not necessarily written back to back, but this pair is expected to land
        // together within one boot.
        if (reg == FS_REG_IHW_LO) {
            s_fs_ihw = (uint16_t)((s_fs_ihw & 0xFF00u) | val);
        } else {
            s_fs_ihw = (uint16_t)((s_fs_ihw & 0x00FFu) | ((uint16_t)val << 8));
        }
        s_fs_ihw_epoch++;
    } else if (addr == ADDR_ENC_FS && reg == FS_REG_IHO_LO) {
        // See FS_REG_IHO_LO above -- latch every write, same reasoning as WSS_DATAF1
        // (expected to change live if the GPU rewrites it per mode switch).
        s_fs_iho = val;
        s_fs_iho_valid = true;
        s_fs_ihw_epoch++;
    } else if (addr == ADDR_ENC_FS && reg == FS_REG_WSS_DATAF1_B0) {
        // Latch every write, not just a first-seen one -- expected to change live if
        // the GPU rewrites it per mode/title.
        s_fs_wss_b0 = val;
        s_fs_wss_valid = true;
    } else if (addr == ADDR_ENC_FS && reg == FS_REG_WSS_DATAF1_B2) {
        s_fs_wss_b2 = val;
    }

    // Xcalibur block writes: 4 data bytes, LSByte first, all traced under the block's
    // register (the ISR already dropped the length byte). A new register, or a fifth byte
    // under the same one, starts the next value.
    if (addr == ADDR_ENC_XCAL) {
        if (reg != s_xcal_acc_reg || s_xcal_acc_n >= 4) {
            s_xcal_acc_reg = reg;
            s_xcal_acc_n = 0;
        }
        s_xcal_acc = (s_xcal_acc >> 8) | ((uint32_t)val << 24);
        if (++s_xcal_acc_n < 4) return;
        if (reg == XCAL_REG_IN_RASTER) {
            s_xcal_in_raster = s_xcal_acc;
        } else if (reg == XCAL_REG_WIDTH) {
            s_xcal_width = s_xcal_acc;
        } else if (reg == XCAL_REG_OUT_RASTER) {
            s_xcal_out_raster = s_xcal_acc;
        } else if (reg == XCAL_REG_MODE_SEL) {
            s_xcal_mode_index = xcal_derive_mode_index((uint8_t)s_xcal_acc);
        } else if (reg == XCAL_REG_ASPECT) {
            s_xcal_aspect = s_xcal_acc;
            s_xcal_aspect_valid = true;
        }
    }
}

// Decode every traced write not decoded yet. Called each supervisor tick and by each getter
// below, so a reader never sees a value older than what the ISR has framed. Main loop only.
// If the ISR lapped the decoder (the main loop blocked through more than ENC_TRACE_N bytes),
// the overwritten entries are counted in `lost` (encdump) and Xcalibur block assembly
// restarts (a gap inside one block followed by another write of that same register would
// still misalign that one value; the next mode set rewrites them all).
void smbus_probe_drain(void)
{
    const uint16_t head = s_enc_tr_head;
    if ((uint16_t)(head - s_enc_dec_tail) > ENC_TRACE_N) {
        const uint16_t lost = (uint16_t)(head - s_enc_dec_tail - ENC_TRACE_N);
        s_enc_dec_lost = (uint16_t)((s_enc_dec_lost + lost < s_enc_dec_lost) ? 0xFFFF : s_enc_dec_lost + lost);
        s_enc_dec_tail = (uint16_t)(head - ENC_TRACE_N);
        s_xcal_acc_n = 0;
        s_xcal_acc_reg = 0;
    }
    while (s_enc_dec_tail != head) {
        const uint16_t i = s_enc_dec_tail & (ENC_TRACE_N - 1);
        enc_decode(s_enc_tr_addr[i], s_enc_tr_reg[i], s_enc_tr_val[i]);
        s_enc_dec_tail++;
    }
}

// Called from smbus_probe_scl_edge() below -- same rising-SCL edge, second listener. Kept to
// framing and one ring append: this runs inside the 9th bit's low phase budget (see
// ENC_TRACE_N), so nothing here may grow.
static void eeprom_probe_scl_edge(void)
{
    const eeprom_probe_state_t st = eeprom_state;
    if (st == EEPROM_ST_IDLE || st == EEPROM_ST_PARK) return;
    if (st == EEPROM_ST_ADDR_ACK) {
        eeprom_state = eeprom_is_write ? EEPROM_ST_OFFSET : EEPROM_ST_DATA;
        return;
    }
    if (st == EEPROM_ST_OFFSET_ACK) {
        // ACK clock after the register-select byte. The EEPROM read follows with a repeated
        // START (parks); an encoder write has its data bytes right after.
        eeprom_byte_in_txn = 0;
        eeprom_state = (eeprom_addr == ADDR_EEPROM_W) ? EEPROM_ST_PARK : EEPROM_ST_DATA;
        return;
    }
    if (st == EEPROM_ST_DATA_ACK) {
        eeprom_state = EEPROM_ST_DATA;
        return;
    }

    // EEPROM_ST_ADDR / EEPROM_ST_OFFSET / EEPROM_ST_DATA: shifting in a byte, MSB first.
    const uint8_t b = (uint8_t)((eeprom_shiftreg << 1) | ((GPIOB->IDR & GPIO_PIN_11) ? 1 : 0));
    eeprom_shiftreg = b;
    if (++eeprom_bitcount < 8) return;
    eeprom_bitcount = 0;

    if (st == EEPROM_ST_ADDR) {
        if (b == ADDR_EEPROM_W || b == ADDR_EEPROM_R || b == ADDR_ENC_CX || b == ADDR_ENC_FS ||
            b == ADDR_ENC_XCAL || b == ADDR_SMC) {
            eeprom_addr = b;
            eeprom_is_write = (b != ADDR_EEPROM_R);
            eeprom_byte_in_txn = 0;   // a read continues from the offset its write selected
            eeprom_state = EEPROM_ST_ADDR_ACK;
        } else {
            eeprom_state = EEPROM_ST_IDLE; // not a device we watch -- ignore this transaction
        }
        return;
    }
    if (st == EEPROM_ST_OFFSET) {
        eeprom_cur_offset = b;
        eeprom_state = EEPROM_ST_OFFSET_ACK;
        return;
    }

    // EEPROM_ST_DATA. The subaddress auto-increments per byte on the Xbox EEPROM read and on
    // Conexant/Focus writes (CX datasheet: "internal address counter will automatically
    // increment after each write"). Xcalibur writes are SMBus block writes -- subaddr, a 0x04
    // length byte, 4 little-endian data bytes (Xbox Linux xcalibur_i2c_write_block()) -- so
    // the subaddr stays fixed. The one exception is 0x9E, which X-HD-aware BIOSes write one
    // byte per transaction (the mode dword they also send X-HD on opcode 131): its lone byte
    // lands in the length slot and is dropped, so it is never traced or decoded.
    eeprom_state = EEPROM_ST_DATA_ACK;
    const uint8_t k = eeprom_byte_in_txn++;
    const uint8_t a = eeprom_addr;
    if (a == ADDR_EEPROM_R) {
        const uint16_t reg = (uint16_t)eeprom_cur_offset + k;
        if (reg >= XBOX_EEPROM_VIDEO_STANDARD_OFFSET && reg < XBOX_EEPROM_VIDEO_STANDARD_OFFSET + 4) {
            s_eeprom_video_standard[reg - XBOX_EEPROM_VIDEO_STANDARD_OFFSET] = b;
            if (reg == XBOX_EEPROM_VIDEO_STANDARD_OFFSET + 3) s_eeprom_video_standard_valid = true;
        }
        if (reg >= XBOX_EEPROM_VIDEO_SETTINGS_OFFSET && reg < XBOX_EEPROM_VIDEO_SETTINGS_OFFSET + 4) {
            s_eeprom_video_settings[reg - XBOX_EEPROM_VIDEO_SETTINGS_OFFSET] = b;
            if (reg == XBOX_EEPROM_VIDEO_SETTINGS_OFFSET + 3) s_eeprom_video_settings_valid = true;
        }
        return;
    }
    const bool xcal_block = (a == ADDR_ENC_XCAL);
    if (xcal_block && k == 0) return;   // the block-length prefix, not a register value
    const uint16_t head = s_enc_tr_head;
    const uint16_t slot = head & (ENC_TRACE_N - 1);
    s_enc_tr_addr[slot] = a;
    s_enc_tr_reg[slot]  = xcal_block ? eeprom_cur_offset : (uint8_t)(eeprom_cur_offset + k);
    s_enc_tr_val[slot]  = b;
    s_enc_tr_head = (uint16_t)(head + 1);
    if (s_enc_tr_total < 0xFFFF) s_enc_tr_total++;
}

// Called from smbus_probe_sda_edge() below -- same framing edge, second listener. A STOP
// clears transaction-level state (offset/byte-position) too, unlike the encoder probe's SDA
// handler which only ever needs to reset a single in-flight byte.
//
// SDA edges in the ACK slot are ignored: the master releasing SDA after the 8th bit and the
// slave pulling it low both happen while SCL is low, but an edge serviced after the ACK
// clock has already risen reads as a START or STOP -- and no START/STOP can legally come
// there. The same lateness after the ACK (the slave releasing SDA as the next bit's clock
// rises) would read as a STOP; the re-sample below rejects the ones where SCL already fell.
static void eeprom_probe_sda_edge(void)
{
    const eeprom_probe_state_t st = eeprom_state;
    if (st == EEPROM_ST_ADDR_ACK || st == EEPROM_ST_OFFSET_ACK || st == EEPROM_ST_DATA_ACK) return;
    if (!(GPIOB->IDR & GPIO_PIN_10)) return; // only meaningful while SCL is high
    if (GPIOB->IDR & GPIO_PIN_11) {
        __NOP(); __NOP(); __NOP(); __NOP();
        if (!(GPIOB->IDR & GPIO_PIN_10)) return; // SCL already fell -- spurious, not a STOP
        eeprom_state = EEPROM_ST_IDLE; // rising while SCL genuinely still high = STOP
        eeprom_bitcount = 0;
    } else {
        eeprom_bitcount = 0;           // falling while SCL high = START (fresh or repeated)
        eeprom_shiftreg = 0;
        eeprom_state = EEPROM_ST_ADDR;
    }
}

// SCL edge (rising) -- sample SDA as the next address bit, MSB first. Doesn't set
// probe_activity_seen -- see that function's own comment for why SCL alone doesn't prove
// this probe's SDA tap is actually connected to anything.
void smbus_probe_scl_edge(void)
{
    // The always-armed EEPROM/encoder-format state machine (below) runs every edge.
    if (s_eeprom_probe_armed) eeprom_probe_scl_edge();

    // The encoder-ID sniffer resolves once and stops. The EXTI stays on for the format probe,
    // so it needs its own gate -- otherwise every encoder address on the bus (a BIOS scans all
    // three at boot) overwrote probe_found_addr.
    if (!s_encoder_probe_armed) return;

    if (probe_bitcount == 9) {
        // Clock right after the 8 address bits = the ACK/NAK. A *present* encoder pulls SDA
        // low (ACK); a scan of an absent encoder's address gets NAK (SDA stays high). Only
        // latch an address the slave actually acknowledged -- this is what filters a
        // "probe all three" boot scan down to the one chip that's really fitted.
        if (!(GPIOB->IDR & GPIO_PIN_11)) {  // SDA low = ACK
            probe_found_addr = probe_pending_addr;
            smbus_disarm_encoder_probe();   // resolved -- this sniffer is done for the session
        }
        probe_bitcount = 0xFF;
        return;
    }

    if (probe_bitcount >= 8) return;
    probe_shiftreg = (uint8_t)((probe_shiftreg << 1) | ((GPIOB->IDR & GPIO_PIN_11) ? 1 : 0));
    if (++probe_bitcount == 8) {
        if (probe_shiftreg == ADDR_ENC_CX || probe_shiftreg == ADDR_ENC_FS || probe_shiftreg == ADDR_ENC_XCAL) {
            probe_pending_addr = probe_shiftreg;
            probe_bitcount = 9;   // hold for the ACK bit on the next SCL edge
        } else {
            probe_bitcount = 0xFF;
        }
    }
}

// SDA edge -- only a genuine START/STOP if SCL is currently high (an SDA change while SCL
// is low is just a normal data bit, not a framing condition).
void smbus_probe_sda_edge(void)
{
    probe_activity_seen = true;
    if (s_eeprom_probe_armed) eeprom_probe_sda_edge();

    if (probe_bitcount == 9) return;        // the ACK slot -- see eeprom_probe_sda_edge()
    if (!(GPIOB->IDR & GPIO_PIN_10)) return;
    if (GPIOB->IDR & GPIO_PIN_11) {
        probe_bitcount = 0xFF; // rising while SCL high = STOP
    } else {
        probe_bitcount = 0;    // falling while SCL high = START
        probe_shiftreg = 0;
    }
}
// -------------------- Initialization --------------------
// The passive PB10/PB11 edge probe (EXTI routing, triggers, arm), set up first thing in
// main(): the board powers up with the console, and the encoder traffic, the EEPROM read
// (~1-2 s in) and some BIOSes' only Conexant IN_MODE write happen once, early. Needs only
// the GPIOB/SYSCFG clocks and EXTI4_15 (enabled by main() as ADV_IRQ_CHANNEL; priority 2,
// below I2C2's 1, so the probes can stay armed); the I2C2 slave itself is set up afterwards
// in smbus_i2c_init(). The supervisor disarms the encoder probe once resolved or after
// 45 s / 180 s; the `pr` CLI command re-arms it.
void smbus_probe_early_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    // PB10/PB11 as plain pulled-up inputs so the EXTI input path is live. smbus_i2c_init()
    // later switches them to AF1 open-drain for the I2C2 peripheral -- EXTI edge detection
    // samples the raw pin level and keeps working across that.
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    // EXTI by register, not HAL_GPIO_Init(GPIO_MODE_IT_x): that sets MODER, which would undo
    // I2C2's AF1 if it ran after smbus_i2c_init().
    SYSCFG->EXTICR[2] = (uint16_t)((SYSCFG->EXTICR[2] & ~((0xFu << 8) | (0xFu << 12))) | (0x1u << 8) | (0x1u << 12));
    EXTI->RTSR |= (1u << 10) | (1u << 11); // SCL sample point + SDA STOP (rising, SCL high)
    EXTI->FTSR |= (1u << 11);              // SDA START (falling, SCL high)

    smbus_arm_encoder_probe();
    smbus_arm_eeprom_probe();
}


// -------------------- Cross-file notifications from the I2C2 slave --------------------
// Fed by HAL_I2C_AddrCallback (smbus_i2c.c): being addressed at 0x69 is SMBus activity too,
// even with the edge probe disarmed (the LED logic reads smbus_probe_activity_seen()).
void smbus_note_probe_activity(void)
{
    probe_activity_seen = true;
}

void smbus_note_host_addressed(void)
{
    s_host_addressed_us = true;
}

