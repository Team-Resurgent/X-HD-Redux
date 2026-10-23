#include "adv7511_edid.h"
#include "adv7511.h"
#include "adv7511_i2c.h"
#include "debug.h"
#include <string.h>

// The ADV7511's internal EDID RAM (read via edid_read_256() below, I2C address 0x7E) is
// only auto-populated once, on HPD assertion after power-up -- it is NOT re-fetched from
// the sink on later HPD toggles (Programming Guide 4.5.1.1: "EDID is not re-read after
// HDCP initialization"). Each failed read (I2C NACK -- e.g. DDC not ready yet, or a flaky
// connection) decrements the EDID Tries register (0xC9[3:0], default 3); once it hits 0
// the chip stops attempting reads entirely until that register is rewritten non-zero.
// DDC Controller State (0xC8[3:0], Programming Guide Table 11): 1 = reading EDID.
static bool edid_controller_reading(void)
{
    return (adv7511_read_register(0xC8) & 0x0F) == 0x01;
}

static void adv7511_edid_request_reread(void)
{
    // Clear any stale EDID Ready flag so we can tell a fresh completion apart from one
    // left over from before. 0x96 is write-1-to-clear: write only this bit. A
    // read-modify-write would also ack a latched HPD flag (0x96[7]), and then
    // adv_handle_interrupts() would never power the chip back up after that blip.
    adv7511_write_register(0x96, 0b00000100);

    // 0xC9[4]=EDID Reread, 0xC9[3:0]=EDID Tries. Reset Tries to the datasheet default (3)
    // in case a prior connection exhausted it, and toggle Reread 0->1 for 10 consecutive
    // writes (datasheet 4.5.1.4: required for a reliably-captured reread request).
    for (uint8_t i = 0; i < 10; i++) {
        adv7511_write_register(0xC9, 0b00000011);
        adv7511_write_register(0xC9, 0b00010011);
    }

    // Poll for EDID Ready (0x96[2]) rather than guessing a fixed settle time -- the
    // reread is an async background DDC transaction on the chip's side. Ready alone is not
    // enough: toggling Main Power Down also starts a read (Programming Guide 4.5.1.4), and
    // adv7511_reinit_chip()'s power-up runs just before the first fetch at boot -- that read
    // completing set Ready while our reread was still reloading the EDID memory ("[EDID] blk0
    // bad header" on most boots, then "retry ok" 100ms later). So also wait for the
    // controller to leave "reading EDID": only then is the memory complete.
    for (uint8_t t = 0; t < 20; t++) {
        if ((adv7511_read_register(0x96) & 0b00000100) && !edid_controller_reading()) {
            return;
        }
        HAL_Delay(10);
    }
    // Timed out: both callers carry on with whatever EDID RAM already holds. Not logged --
    // a stale read that then fails shows up via edid_read_256()'s own messages, and one
    // that succeeds is fine.
}

static bool edid_read_256(uint8_t *buf)
{
    memset(buf, 0, 256);
    I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();

    // Read Block 0 (128 bytes)
    if (HAL_I2C_Mem_Read(hi2c, 0x7E, 0x00, I2C_MEMADD_SIZE_8BIT, buf, 128, 60) != HAL_OK) {
        debug_log("[EDID] blk0 read failed\r\n");
        return false;
    }

    // Check EDID Header 00 FF FF FF FF FF FF 00
    if (buf[0] != 0x00 || buf[1] != 0xFF || buf[2] != 0xFF || buf[3] != 0xFF ||
        buf[4] != 0xFF || buf[5] != 0xFF || buf[6] != 0xFF || buf[7] != 0x00) {
        // C8 = DDC controller error [7:4] / state [3:0] (Programming Guide Tables 11/12),
        // 96 = interrupt flags (EDID Ready is bit 2) -- what the controller was doing when
        // the memory read back wrong.
        debug_log("[EDID] blk0 bad header (C8=%02X 96=%02X)\r\n",
                  adv7511_read_register(0xC8), adv7511_read_register(0x96));
        return false;
    }

    // Block 0 checksum: sum of all 128 bytes must be 0 mod 256 per the EDID spec.
    // A torn/partial I2C read can still pass the header check above while carrying
    // corrupted data, and that data feeds directly into HDMI/color-space decisions.
    uint8_t checksum = 0;
    for (uint16_t i = 0; i < 128; i++) {
        checksum += buf[i];
    }
    if (checksum != 0) {
        debug_log("[EDID] blk0 csum %02X\r\n", checksum);
        return false;
    }

    // Read Block 1 if extension exists
    uint8_t ext_count = buf[126];
    if (ext_count > 0) {
        if (HAL_I2C_Mem_Read(hi2c, 0x7E, 128, I2C_MEMADD_SIZE_8BIT, &buf[128], 128, 60) != HAL_OK) {
            debug_log("[EDID] ext read failed\r\n");
            memset(&buf[128], 0, 128);
        } else {
            uint8_t ext_checksum = 0;
            for (uint16_t i = 128; i < 256; i++) {
                ext_checksum += buf[i];
            }
            if (ext_checksum != 0) {
                debug_log("[EDID] ext csum %02X\r\n", ext_checksum);
                memset(&buf[128], 0, 128);
            }
        }
    }

    return true;
}

typedef struct {
    bool valid;
    bool is_hdmi;
    bool supports_yuv444;
    bool supports_underscan;
    uint16_t phys_addr;      // CEC physical address A.B.C.D from the HDMI VSDB, 0xFFFF = none
} edid_cached_info_t;

static edid_cached_info_t cached_edid = {0};
static uint8_t s_edid_buf[256];

typedef void (*edid_block_cb)(uint8_t tag, uint8_t len, const uint8_t *payload, void *ctx);

// Walks the CEA-861 Data Block Collection (EDID extension byte 128==0x02, DTD start at
// byte 130), invoking cb(tag, len, payload, ctx) for each block -- payload points at the
// byte right after the tag/len header. Bounds-checked so a malformed or truncated block
// can't read past either the data block collection or the physical buffer (256 bytes).
static void edid_walk_data_blocks(const uint8_t *buf, edid_block_cb cb, void *ctx)
{
    if (buf[128] != 0x02) return;
    uint8_t dtd_start = buf[130];
    // dtd_start==0 is CTA-861-G/H sec 7.5's sentinel for "no DTDs in this extension," not a
    // malformed offset -- the Data Block Collection then extends to the extension's last
    // byte before the checksum (127). Common for AVRs/capture devices that put every timing
    // in Block 0 and use the CEA extension purely for audio/HDMI VSDB. Nonzero values 1-3
    // are still rejected as too small to be a real offset (the DBC always starts at byte 4).
    if (dtd_start != 0 && dtd_start < 4) return;
    uint8_t dtd_end = (dtd_start == 0) ? 127 : dtd_start;
    if (dtd_end > 127) return;

    uint16_t idx = 132;
    uint16_t end = 128 + dtd_end;
    while (idx < end && idx < 255) {
        uint8_t header = buf[idx];
        uint8_t tag = (header >> 5) & 0x07;
        uint8_t len = header & 0x1F;
        if (idx + 1 + len > end || idx + 1 + len > 256) {
            break;
        }
        cb(tag, len, &buf[idx + 1], ctx);
        idx += 1 + len;
    }
}

// The HDMI VSDB (IEEE OUI 00-0C-03): its presence makes the sink HDMI, and its next two bytes
// are the sink-assigned CEC physical address (A.B.C.D, one nibble each).
static void edid_cache_hdmi_cb(uint8_t tag, uint8_t len, const uint8_t *p, void *ctx)
{
    edid_cached_info_t *info = (edid_cached_info_t *)ctx;
    if (tag == 3 && len >= 3 && p[0] == 0x03 && p[1] == 0x0C && p[2] == 0x00) {
        info->is_hdmi = true;
        if (len >= 5) info->phys_addr = (uint16_t)((p[3] << 8) | p[4]);
    }
}

// Set alongside cached_edid.valid on a total read failure -- distinct from `valid` (which
// stays true so callers get the safe fallback immediately, not garbage/uninitialized
// state) so edid_retry_periodic() below knows there's a real reason to keep trying, rather
// than treating every failure as final. Cleared the moment a read actually succeeds.
static bool s_edid_read_failed = false;
static uint32_t s_edid_last_retry_tick = 0;
// When the current run of failures began. A sink that raises HPD before its DDC is ready (an
// HDMI switch / AVR) answers within EDID_OUTPUT_HOLD_MS; until then the read is retried every
// EDID_FAST_RETRY_MS and output is held off (edid_output_hold()), so the mode goes out once
// with the right HDMI/DVI configuration instead of DVI first, then again as HDMI.
static uint32_t s_edid_fail_since = 0;
#define EDID_FAST_RETRY_MS     100UL
#define EDID_OUTPUT_HOLD_MS    1500UL
// Rate limit for edid_retry_periodic() once the output hold is over -- a permanently-dead DDC
// wire (no display at all) would otherwise retry on every main-loop pass, each one a real
// up-to-200ms blocking I2C sequence.
#define EDID_RETRY_INTERVAL_MS 1000UL

void edid_update_cache(void)
{
    // Main Power Down (0x41[6], its POR state until HPD+MSEN power the chip up) holds the DDC
    // controller in reset: there is nothing to read, and trying cost a 200ms poll and a
    // "[EDID] blk0 bad header (C8=00 ...)" on boots where a lazy getter ran first (the CLI
    // `d` extras via the effective colour space, SMBus video info). Leave the cache invalid;
    // adv7511_reinit_chip() fetches it right after powering the chip up.
    if (adv7511_read_register(0x41) & 0x40) {
        return;
    }
    adv7511_edid_request_reread();

    if (!edid_read_256(s_edid_buf)) {
        // Total read failure (dead DDC wire, unpowered switch, DVI-D monitor with no DDC,
        // or an AVR/HDMI-switch that raised HPD before its own DDC EEPROM rail was ready).
        // Fall back to the safe unknown-sink default: DVI signalling, RGB full range, no
        // YUV444 -- every HDMI and DVI display can show that without a handshake, whereas
        // assuming HDMI/YUV444 puts a magenta tint and audio-packet noise on a DVI sink.
        // edid_update_cache() only runs on an HPD/MSEN edge, so a sink that holds HPD
        // stable through its own slow DDC power-up would otherwise stay muted until a
        // physical replug; edid_retry_periodic() below retries independently of that edge.
        debug_log("[EDID] read failed, DVI/RGB fallback\r\n");
        // The raw copy SMBus serves (READ_EDID) says the same: no EDID. A failed check leaves
        // a partial or garbled block in the buffer, and keeping an earlier good one would
        // describe what may be a different display.
        memset(s_edid_buf, 0, sizeof s_edid_buf);
        cached_edid.valid = true;
        cached_edid.is_hdmi = false;
        cached_edid.phys_addr = 0xFFFF;
        cached_edid.supports_yuv444 = false;
        cached_edid.supports_underscan = true;
        if (!s_edid_read_failed) {
            s_edid_fail_since = HAL_GetTick();
        }
        s_edid_read_failed = true;
        s_edid_last_retry_tick = HAL_GetTick();
        return;
    }
    s_edid_read_failed = false;
    cached_edid.valid = true;
    cached_edid.is_hdmi = false;
    cached_edid.phys_addr = 0xFFFF;
    edid_walk_data_blocks(s_edid_buf, edid_cache_hdmi_cb, &cached_edid);
    cached_edid.supports_underscan = (s_edid_buf[128] == 0x02 && (s_edid_buf[131] & 0x80));
    // YCbCr needs HDMI: a sink without the HDMI VSDB gets DVI mode (0xAF[1]=0) and no AVI
    // InfoFrame to say the stream is YCbCr, so it decodes it as RGB (magenta picture). A
    // CEA extension alone (byte 131 bit 5) can still advertise 4:4:4 on such a sink.
    cached_edid.supports_yuv444 = cached_edid.is_hdmi &&
                                  (s_edid_buf[128] == 0x02 && (s_edid_buf[131] & 0x20));
}

// True while output is held for a failed read that might still succeed (EDID_OUTPUT_HOLD_MS).
bool edid_output_hold(void)
{
    return s_edid_read_failed && (HAL_GetTick() - s_edid_fail_since) < EDID_OUTPUT_HOLD_MS;
}

// Every main-loop pass (the supervisor). Retries a failed read -- every EDID_FAST_RETRY_MS
// during the output hold, then every EDID_RETRY_INTERVAL_MS -- and is a no-op otherwise. True
// on the pass a retry succeeds, so the caller can restore HDMI mode and audio.
bool edid_retry_periodic(void)
{
    if (!s_edid_read_failed) {
        return false;
    }
    const uint32_t interval = edid_output_hold() ? EDID_FAST_RETRY_MS : EDID_RETRY_INTERVAL_MS;
    if ((int32_t)(HAL_GetTick() - s_edid_last_retry_tick) < (int32_t)interval) {
        return false;
    }
    edid_update_cache();
    return !s_edid_read_failed;
}

// No read of its own (unlike the getters below): 0xFFFF until a read found the HDMI VSDB.
uint16_t edid_cec_physical_address(void)
{
    return cached_edid.valid ? cached_edid.phys_addr : 0xFFFF;
}

bool edid_is_hdmi_sink(void)
{
    if (!cached_edid.valid) edid_update_cache();
    return cached_edid.is_hdmi;
}

bool edid_supports_yuv444(void)
{
    if (!cached_edid.valid) edid_update_cache();
    return cached_edid.supports_yuv444;
}

bool edid_supports_underscan(void)
{
    if (!cached_edid.valid) edid_update_cache();
    return cached_edid.supports_underscan;
}

// Raw 256-byte EDID (both blocks) for a caller (e.g. a console-side dashboard, over SMBus)
// to parse independently, rather than being limited to whatever subset of
// fields X-HD itself decodes into cached_edid.
//
// Deliberately no lazy edid_update_cache() fallback here, unlike the getters above: this is
// the one EDID accessor reachable from an I2C ISR context (HAL_I2C_SlaveRxCpltCallback in
// smbus_i2c.c, answering the console's BIOS/dashboard in real time).
// adv7511_edid_request_reread() blocks for up to ~200ms, which would stall the SMBus
// response inside an ISR. s_edid_buf is a plain static array (.bss, zero-initialized before
// main() runs), so a read landing before the first successful edid_update_cache() returns
// an all-zero, invalid-but-not-corrupt EDID -- as does one after a failed read. Filled by
// adv7511_reinit_chip()'s first fetch and refreshed on every HPD/MSEN reconnect and retry.
const uint8_t* edid_get_raw_buffer(void)
{
    return s_edid_buf;
}
