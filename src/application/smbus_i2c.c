#include "smbus_i2c.h"
#include "../shared/adv7511_i2c.h"
#include "xbox_video_bios.h"
#include "stm32.h"
#include "flash.h"
#include "../shared/crc32.h"
#include "../shared/smbus_slave_common.h"
#include "../shared/debug.h"
#include "../shared/defines.h"
#include <string.h>

#include "../shared/adv7511_xbox.h"
#include "../shared/adv7511_edid.h"
#include "../shared/xbox_video_timing.h"
#include "../shared/user_config.h"

static SMBusSettings scratchSettings = {0};
static SMBusSettings settings = {0};

static uint16_t config_buffer_bank = 0;
static uint16_t config_buffer_index = 0;

static uint16_t ram_buffer_bank = 0;
static uint16_t ram_buffer_index = 0;
static uint8_t ram_buffer[RAM_BUFFER_SIZE];
static uint32_t ram_buffer_crc = 0;

// WRITE_RAM_APPLY's actual flash write, deferred out of the I2C2 ISR to main-loop context
// -- see that case's own comment. Consumed by smbus_consume_flash_write(), called once per
// main-loop iteration alongside smbus_consume_settings_changes(). Safe for ram_buffer/CRC
// to be snapshotted here rather than copied: the host updater doesn't start writing a new
// page into ram_buffer until well after it reads back this page's flash-verified CRC,
// which is itself well after main-loop cadence would have already consumed this pending
// write.
static volatile bool s_flash_write_pending = false;
static volatile uint16_t s_flash_write_page = 0;
static volatile uint32_t s_flash_write_crc = 0;

// Video info (opcodes 22/151): main loop writes s_video_live (IRQs off for the copy); the
// latch copies it to the session's video_snap, which READ_VIDEO_INFO streams.
static XhdVideoInfo s_video_live;

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif
// BUILD_DATETIME (inject_git_version.py) is space-free, unlike __DATE__/__TIME__: the
// host's `v` line carries it as one space-separated token.
#ifndef BUILD_DATETIME
#define BUILD_DATETIME "unknown"
#endif
// About block (opcode 23): build strings fixed at compile time, UID copied at init, chip ID
// recorded once the ADV7511 is configured.
static XhdAbout s_about = {
    .struct_version = XHD_ABOUT_VERSION,
    .struct_size = (uint8_t)sizeof(XhdAbout),
    .git = GIT_HASH,
    .built = BUILD_DATETIME,
};

void smbus_note_chip_id(uint8_t id, uint8_t rev)
{
    s_about.chip_id = id;
    s_about.chip_rev = rev;
}


// SPD name staging (opcodes 21/149/150, see defines.h). The ISR only validates and copies
// the 24 bytes into user_config; the ADV7511 packet rewrite (multi-write I2C1, plus a
// possibly-blocking first EDID read in edid_is_hdmi_sink()) is deferred to the main loop
// via s_spd_apply_pending -- same reason colour space uses color_space_reapply_pending.
static volatile bool s_spd_apply_pending = false;

// Telemetry (opcode 15). Written only from main-loop context (publish/event/note below),
// read byte-by-byte by the slave ISR via the cursor. Fields are naturally aligned so the
// ISR never sees a torn multi-byte value -- see the note in smbus_i2c.h.
static XhdTelemetry s_telemetry = {
    .struct_version = XHD_TELEMETRY_VERSION,
    .struct_size    = (uint8_t)sizeof(XhdTelemetry),
};

// Per-session state: the console's SMBus and the UART bridge (debug.c `sb`) each have their
// own read cursors, staging buffers and pending changes, so neither can corrupt the other's
// multi-step sequences, and the bridge never has to mask the SMBus interrupt. What they share
// is the device itself: the settings, the live video info, telemetry and about block.
typedef struct {
    uint8_t edid_idx;                    // READ_EDID cursor (WRITE_EDID_INDEX); wraps at 256
    uint8_t about_idx;                   // READ_ABOUT cursor
    uint8_t tlm_idx;                     // READ_TELEMETRY cursor
    uint8_t cfg_idx;                     // READ_USER_CONFIG cursor
    uint8_t video_idx;                   // READ_VIDEO_INFO cursor into video_snap
    XhdVideoInfo video_snap;             // WRITE_VIDEO_INFO_LATCH's copy of s_video_live
    uint8_t spd_stage[XHD_SPD_NAME_LEN]; // WRITE_SPD_DATA staging
    uint8_t spd_w, spd_r;                // SPD staging / read cursors
    uint8_t cec_tx[ADV7511_CEC_MAX_FRAME]; // WRITE_CEC_TX_DATA staging; TX_APPLY queues it
    uint8_t cec_tx_n;                    // CEC Tx staging length
    uint8_t cec_rx_idx;                  // READ_CEC_RX_DATA cursor
    // Changes handed to the main loop (smbus_consume_settings_changes()).
    volatile uint16_t dirty;             // SM_DIRTY_*
    volatile uint8_t phase;              // WRITE_PHASE
    volatile int8_t pos_h, pos_v;        // WRITE_POSITION_H/V
    volatile uint8_t pos_pending;        // bit 0 = H, bit 1 = V
    volatile uint8_t audio_mute;         // WRITE_AUDIO_MUTE
} smbus_session_t;
static smbus_session_t s_smbus;          // the console (I2C2 ISR)
static smbus_session_t s_uart;           // the UART bridge (main loop)

static volatile bool video_mode_update_pending = false;
static volatile bool color_space_reapply_pending = false;
// Incremented each time the ISR commits a new mode apply. bios_loop() snapshots
// this before its long TMDS/PLL settle sequence so it can tell, once that
// sequence finishes, whether a newer command arrived while it was busy --
// otherwise acking unconditionally can clear the pending flag a newer ISR
// write just set, silently dropping that update.
static volatile uint32_t update_sequence = 0;
static bool g_smbus_sniff_enabled = false;

// --- Control suite (opcode 13 read, 141-145 write) ---
// The SMBus ISR only ever touches RAM here: reads come from user config, writes stash a value
// into user config and raise a dirty bit. Every I2C1 register write, flash save and mode
// reapply is done off-ISR by smbus_consume_settings_changes(), so a slave interrupt can never
// reenter I2C1 mid main-loop transaction. (Opcode 14 is retired; its status bits are in the
// video info flags, opcode 22.)
#define SM_DIRTY_PHASE    0x01
#define SM_DIRTY_ASPECT   0x02
#define SM_DIRTY_SCAN     0x04
#define SM_DIRTY_SAVE     0x08
#define SM_DIRTY_DEFAULTS 0x10
#define SM_DIRTY_REAPPLY  0x20
#define SM_DIRTY_WVIC     0x40
#define SM_DIRTY_POSITION 0x80
#define SM_DIRTY_RELOAD   0x100
#define SM_DIRTY_AUDIO    0x200

typedef struct {
    uint8_t is_write;
    uint8_t cmd;
    uint8_t val;
} SniffEvent;

#define SNIFF_QUEUE_SIZE 64
static SniffEvent sniff_queue[SNIFF_QUEUE_SIZE];
static volatile uint8_t sniff_head = 0;
static volatile uint8_t sniff_tail = 0;


void smbus_set_sniff(bool enable)
{
    g_smbus_sniff_enabled = enable;
    if (!enable) {
        sniff_head = sniff_tail = 0;
    }
}

bool smbus_get_sniff(void)
{
    return g_smbus_sniff_enabled;
}

// Freeze is the page-63 smbus_freeze byte (WRITE_FREEZE): live, and applied at boot once saved.
bool smbus_get_apply_frozen(void)
{
    return user_config_get()->smbus_freeze != 0;
}

// Shared by the read-response and write-data sniff-recording sites below.
static void sniff_record(uint8_t is_write, uint8_t cmd, uint8_t val)
{
    if (!g_smbus_sniff_enabled) return;
    uint8_t next = (uint8_t)((sniff_head + 1) % SNIFF_QUEUE_SIZE);
    if (next != sniff_tail) {
        sniff_queue[sniff_head].is_write = is_write;
        sniff_queue[sniff_head].cmd = cmd;
        sniff_queue[sniff_head].val = val;
        sniff_head = next;
    }
}

void smbus_poll_sniffer(void)
{
    while (sniff_head != sniff_tail) {
        SniffEvent ev = sniff_queue[sniff_tail];
        sniff_tail = (uint8_t)((sniff_tail + 1) % SNIFF_QUEUE_SIZE);
        if (ev.is_write) {
            debug_log("[SNIFF] W %02X(%u)=%02X\r\n", ev.cmd, ev.cmd, ev.val);
        } else {
            debug_log("[SNIFF] R %02X(%u)>%02X\r\n", ev.cmd, ev.cmd, ev.val);
        }
    }
}

// -------------------- Initialization --------------------
void smbus_i2c_init(void)
{
    memcpy(s_about.mcu_uid, (const void *)0x1FFFF7AC, sizeof s_about.mcu_uid);   // STM32F0 UID
    // The passive probe's EXTI on these pins was set up by smbus_probe_early_init() (main()
    // runs it first) and keeps working across this switch to AF1.
    smbus_slave_init();
    debug_log("SMBus: slave %02X ready\r\n", I2C_SLAVE_ADDR);
}

// The byte at a block's read cursor; the cursor advances and wraps at the block's size.
static uint8_t read_cursor(const void *block, uint8_t *idx, uint8_t size)
{
    const uint8_t r = ((const uint8_t *)block)[*idx];
    if (++*idx >= size) *idx = 0;
    return r;
}

// One SMBus read command's response byte: the I2C2 ISR's handler, also called by the UART
// bridge (smbus_bridge_read()). `prev` is returned where a command leaves the response as it
// was (an out-of-range config/RAM read), as the ISR always has.
static uint8_t smbus_read_byte(smbus_session_t *s, uint8_t cmd, uint8_t prev)
{
    uint8_t r = prev;
    switch(cmd)
    {
        case I2C_HDMI_COMMAND_READ_CONFIG:
        {
            uint16_t settings_size = sizeof(SMBusSettings);
            uint16_t settings_offset = (config_buffer_bank << 8) | config_buffer_index;
            if (settings_offset >= settings_size)
            {
                break;
            }
            uint8_t *settings_data = (uint8_t*)&settings;
            r = settings_data[settings_offset];
            smbus_paged_index_advance(&config_buffer_bank, &config_buffer_index);
            break;
        }
        case I2C_HDMI_COMMAND_READ_VERSION1:
        case I2C_HDMI_COMMAND_READ_VERSION2:
        case I2C_HDMI_COMMAND_READ_VERSION3:
        case I2C_HDMI_COMMAND_READ_VERSION4:
        {
            static const uint8_t version[4] = {
                I2C_HDMI_VERSION1, I2C_HDMI_VERSION2, I2C_HDMI_VERSION3, I2C_HDMI_VERSION4
            };
            r = version[cmd - I2C_HDMI_COMMAND_READ_VERSION1];
            break;
        }
        case I2C_HDMI_COMMAND_READ_MODE:
        {
            r = I2C_HDMI_MODE_APPLICATION;
            break;
        }
        case I2C_HDMI_COMMAND_READ_RAM:
        {
            uint16_t ram_buffer_offset = (ram_buffer_bank << 8) | ram_buffer_index;
            if (ram_buffer_offset >= RAM_BUFFER_SIZE)
            {
                break;
            }
            r = ram_buffer[ram_buffer_offset];
            smbus_paged_index_advance(&ram_buffer_bank, &ram_buffer_index);
            break;
        }
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC1:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC2:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC3:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4:
        {
            // Big-endian: CRC1 is bits 31:24 ... CRC4 bits 7:0.
            r = (uint8_t)(ram_buffer_crc >> (8u * (I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4 - cmd)));
            break;
        }
        case I2C_HDMI_COMMAND_READ_COLOR_SPACE:
        {
            r = (uint8_t)adv7511_get_color_space();
            break;
        }
        case I2C_HDMI_COMMAND_READ_EDID:
        {
            r = edid_get_raw_buffer()[s->edid_idx];
            s->edid_idx++;
            break;
        }
        case I2C_HDMI_COMMAND_READ_PHASE:
        {
            // The self-latched encoder's calibration slot (0xFF = none yet: slot 0,
            // the same default user_config_get_encoder_index() gives).
            const uint8_t e = user_config_get()->active_encoder;
            r = user_config_get()->enc_cal[e <= 2 ? e : 0].clock_phase;
            break;
        }
        case I2C_HDMI_COMMAND_READ_VIDEO_INFO:
        {
            r = read_cursor(&s->video_snap, &s->video_idx, sizeof(s->video_snap));
            break;
        }
        case I2C_HDMI_COMMAND_READ_ABOUT:
        {
            r = read_cursor(&s_about, &s->about_idx, sizeof(s_about));
            break;
        }
        case I2C_HDMI_COMMAND_READ_TELEMETRY:
        {
            r = read_cursor(&s_telemetry, &s->tlm_idx, sizeof(s_telemetry));
            break;
        }
        case I2C_HDMI_COMMAND_READ_LIVE_FLAGS:
        {
            r = (uint8_t)((adv7511_audio_mute_held() ? 0x01 : 0x00) |
                          (user_config_get()->smbus_freeze ? 0x02 : 0x00));
            break;
        }
        case I2C_HDMI_COMMAND_READ_USER_CONFIG:
        {
            r = read_cursor(user_config_get(), &s->cfg_idx, sizeof(xhd_user_config));
            break;
        }
        case I2C_HDMI_COMMAND_READ_ASPECT_RATIO:
        {
            r = user_config_get()->aspect_ratio;
            break;
        }
        case I2C_HDMI_COMMAND_READ_SCAN_MODE:
        {
            r = user_config_get()->scan_mode;
            break;
        }
        case I2C_HDMI_COMMAND_READ_CEC_STATUS:
        {
            uint8_t rx_len = 0;
            bool rx_pending = adv7511_cec_rx_pending(&rx_len);
            r = (uint8_t)(adv7511_cec_tx_status() | (rx_pending ? 0x10 : 0x00) |
                                     (user_config_get()->cec_clock_off ? 0x00 : 0x20) |
                                     (user_config_get()->cec_one_touch_off ? 0x00 : 0x40) |
                                     (user_config_get()->cec_standby_off ? 0x00 : 0x80));
            break;
        }
        case I2C_HDMI_COMMAND_READ_CEC_RX_LEN:
        {
            uint8_t rx_len = 0;
            adv7511_cec_rx_pending(&rx_len);
            r = rx_len;
            break;
        }
        case I2C_HDMI_COMMAND_READ_SPD_DATA:
        {
            r = read_cursor(user_config_get_spd(), &s->spd_r, XHD_SPD_NAME_LEN);
            break;
        }
        case I2C_HDMI_COMMAND_READ_CEC_RX_DATA:
        {
            uint8_t rx_len = 0, rx_buf[ADV7511_CEC_MAX_FRAME];
            if (adv7511_cec_rx_pending(&rx_len) && s->cec_rx_idx < rx_len)
            {
                adv7511_cec_rx_read(rx_buf);
                r = rx_buf[s->cec_rx_idx];
                s->cec_rx_idx++;
            }
            else
            {
                r = 0;
            }
            break;
        }
        default:
        {
            r = 0x42;
            break;
        }
    }
    return r;
}

// One SMBus write command: the I2C2 ISR's handler, also called by the UART bridge
// (smbus_bridge_write()). Anything slow or chip-touching is handed to the main loop (dirty
// flags), as before -- this runs in the ISR.
static void smbus_write_byte(smbus_session_t *s, uint8_t cmd, uint8_t value)
{
    switch(cmd)
    {
        case I2C_HDMI_COMMAND_WRITE_CONFIG:
        {
            uint16_t settings_size = sizeof(SMBusSettings);
            uint16_t settings_offset = (config_buffer_bank << 8) | config_buffer_index;
            if (settings_offset >= settings_size)
            {
                break;
            }
            uint8_t *scratch_settings_data = (uint8_t*)&scratchSettings;
            scratch_settings_data[settings_offset] = value;
            smbus_paged_index_advance(&config_buffer_bank, &config_buffer_index);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CONFIG_BANK:
        {
            config_buffer_bank = value;
            config_buffer_index = 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CONFIG_INDEX:
        {
             config_buffer_index = value;
             break;
        }
        case I2C_HDMI_COMMAND_WRITE_CONFIG_APPLY:
        {
            if (value == 0x01)
            {
                if (user_config_get()->smbus_freeze)
                {
                    debug_ring_log("SMBus: IGNORED (frozen) mode=%08X\r\n", scratchSettings.mode);
                }
                else
                {
                    memcpy(&settings, &scratchSettings, sizeof(SMBusSettings));
                    video_mode_update_pending = true;
                    update_sequence++;
                    debug_ring_log("SMBus: encoder=%02X region=%02X mode=%08X title=%08X avinfo=%08X\r\n", settings.encoder, settings.region, settings.mode, settings.titleid, settings.avinfo);
                }
            }
            else
            {
                debug_ring_log("SMBus: CONFIG_APPLY bad value=%02X\r\n", value);
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_SET_MODE:
        {
            smbus_set_mode_and_reset(value);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_READ_PAGE:
        {
            // value is caller-controlled (0-255) with no other validation --
            // flash_copy_page()/crc32_copy() dereference it as a raw pointer with
            // no bounds checking of their own. Any page within the physical flash
            // (0-63, including the bootloader's own region -- reading it back for
            // verification is legitimate, unlike writing) is fine; anything higher
            // multiplies out past the actual flash and reads undefined memory.
            if (value >= (FLASH_TOTAL_SIZE >> FLASH_PAGE_SHIFT))
            {
                break;
            }
            ram_buffer_crc = flash_copy_page(value, ram_buffer, RAM_BUFFER_SIZE);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM:
        {
            uint16_t ram_offset = (ram_buffer_bank << 8) | ram_buffer_index;
            if (ram_offset >= RAM_BUFFER_SIZE)
            {
                break;
            }
            ram_buffer[ram_offset] = value;
            smbus_paged_index_advance(&ram_buffer_bank, &ram_buffer_index);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM_BANK:
        {
            ram_buffer_bank = value;
            ram_buffer_index = 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM_INDEX:
        {
            ram_buffer_index = value;
            break;
        }
        // The application writes only the BOOTLOADER's pages (0-19): the mirror
        // image of bootloader/smbus_i2c.c, which writes only application pages
        // (20-63). Each side rewrites the other, never the image it is running
        // from. XHDUpdater's HD+ -> X-HD conversion (updater/src/XHDUpdater/
        // Main.cpp HDPlusProcess()) flashes the X-HD application through the HD+
        // bootloader, switches to Application mode, then calls
        // HDHelper::FlashBootloader() -- pages 0-19 through THIS handler, retrying
        // each page until its read-back CRC matches. 660fe41 flipped this range to
        // 20-63, which made that loop retry forever and let the running
        // application erase its own code. Safe while running: the vector table
        // lives in SRAM (relocate_vector_table_to_ram()), not on page 0.
        // The actual erase/write is deferred to main-loop context
        // (smbus_consume_flash_write(), matching WRITE_CONFIG_APPLY's own pattern)
        // rather than run here in the ISR, since flash_erase_page()/
        // flash_write_page() stall the CPU for the real-world duration of a page
        // erase (tens of ms) during a live Xbox SMBus transaction.
        case I2C_HDMI_COMMAND_WRITE_RAM_APPLY:
        {
            if (value >= (BOOTLOADER_SIZE >> FLASH_PAGE_SHIFT))
            {
                break;
            }
            s_flash_write_page = value;
            s_flash_write_crc = crc32_buf(ram_buffer, RAM_BUFFER_SIZE);
            s_flash_write_pending = true;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_COLOR_SPACE:
        {
            // COLOR_SPACE_MAX (types.h) is the last valid setting.
            if (value <= COLOR_SPACE_MAX)
            {
                adv7511_set_color_space((adv_color_space)value);
                // Mirror into the persisted config so a later user_config_save()
                // (or reboot after one) keeps this choice instead of the stale
                // Page 63 value.
                user_config_get()->color_space = value;
                color_space_reapply_pending = true;
                // The setting's number, not its name: naming Auto resolves it, which can
                // read the EDID over I2C1 -- not from this ISR.
                debug_ring_log("SMBus: color %u\r\n", value);
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_VIDEO_INFO_LATCH:
        {
            if (value == 1)
            {
                s->video_snap = s_video_live;
                s->video_idx = 0;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_WIRE_VIC_MODE:
        {
            if (value <= WIRE_VIC_AUTO)
            {
                user_config_get()->wire_vic_mode = value;
                s->dirty |= SM_DIRTY_WVIC;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CEC_CLOCK_OUT:
        case I2C_HDMI_COMMAND_WRITE_CEC_ONE_TOUCH:
        case I2C_HDMI_COMMAND_WRITE_CEC_STANDBY:
        {
            // The main loop follows all three (cec_clock_out_follow_config(), the
            // supervisor's One Touch Play call and shutdown check).
            if (value <= 1)
            {
                if (cmd == I2C_HDMI_COMMAND_WRITE_CEC_CLOCK_OUT)
                    user_config_get()->cec_clock_off = (uint8_t)!value;   // stored inverted
                else if (cmd == I2C_HDMI_COMMAND_WRITE_CEC_ONE_TOUCH)
                    user_config_get()->cec_one_touch_off = (uint8_t)!value;
                else
                    user_config_get()->cec_standby_off = (uint8_t)!value;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_POSITION_H:
        case I2C_HDMI_COMMAND_WRITE_POSITION_V:
        {
            if (cmd == I2C_HDMI_COMMAND_WRITE_POSITION_H)
            {
                s->pos_h = (int8_t)value;
                s->pos_pending |= 0x01;
            }
            else
            {
                s->pos_v = (int8_t)value;
                s->pos_pending |= 0x02;
            }
            s->dirty |= SM_DIRTY_POSITION;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_AUDIO_MUTE:
        {
            if (value <= 1)
            {
                s->audio_mute = value;
                s->dirty |= SM_DIRTY_AUDIO;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_FREEZE:
        {
            if (value <= 1)
            {
                user_config_get()->smbus_freeze = value;
                // "Simulate disconnected SMBus": drop any real packet that already landed --
                // exactly what a genuinely SMBus-less retail dash would exercise.
                if (value) settings.mode = 0;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_USER_CONFIG_INDEX:
        {
            s->cfg_idx = (value < sizeof(xhd_user_config)) ? value : 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_TELEMETRY_INDEX:
        {
            s->tlm_idx = (value < sizeof(s_telemetry)) ? value : 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_ABOUT_INDEX:
        {
            s->about_idx = (value < sizeof(s_about)) ? value : 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_EDID_INDEX:
        {
            s->edid_idx = value;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_PHASE:
        {
            if (value <= 7)
            {
                s->phase = value;
                s->dirty |= SM_DIRTY_PHASE;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_ASPECT_RATIO:
        {
            if (value <= ASPECT_RATIO_4_3)
            {
                user_config_get()->aspect_ratio = value;
                s->dirty |= SM_DIRTY_ASPECT;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_SCAN_MODE:
        {
            if (value <= SCAN_PC)
            {
                user_config_get()->scan_mode = value;
                s->dirty |= SM_DIRTY_SCAN;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CEC_TX_DATA:
        {
            if (s->cec_tx_n < ADV7511_CEC_MAX_FRAME)
            {
                s->cec_tx[s->cec_tx_n] = value;
                s->cec_tx_n++;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CEC_TX_APPLY:
        {
            if (value >= 1 && value <= s->cec_tx_n)
            {
                adv7511_cec_tx_queue(s->cec_tx, value);
            }
            s->cec_tx_n = 0;   // re-arm staging for the next frame, sent or not
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_SPD_DATA:
        {
            if (s->spd_w < XHD_SPD_NAME_LEN)
            {
                s->spd_stage[s->spd_w++] = value;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_SPD_APPLY:
        {
            // Commit only a complete, valid name -- a partial stage would otherwise
            // splice old and new bytes. user_config_set_spd() rejects non-printable.
            if (value == 0x01 && s->spd_w == XHD_SPD_NAME_LEN &&
                user_config_set_spd(s->spd_stage))
            {
                s_spd_apply_pending = true;
            }
            s->spd_w = 0;   // either way, re-arm for the next name
            s->spd_r = 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CEC_RX_ACK:
        {
            if (value == 0x01)
            {
                adv7511_cec_rx_release();
                s->cec_rx_idx = 0;
            }
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_CONFIG_ACTION:
        {
            if (value == 0x01)
            {
                s->dirty |= SM_DIRTY_SAVE;
            }
            else if (value == 0x02)
            {
                s->dirty |= SM_DIRTY_DEFAULTS;
            }
            else if (value == 0x03)
            {
                s->dirty |= SM_DIRTY_REAPPLY;
            }
            else if (value == 0x04)
            {
                s->dirty |= SM_DIRTY_RELOAD;
            }
            break;
        }
    }
}

// UART bridge (debug.c `sb`): the same handlers the console's SMBus reaches, from the main loop,
// on the UART's own session -- nothing is masked, the console's SMBus always pre-empts it.
// Not bridged: the console's own mode-packet staging/apply and the updater's RAM/flash path
// (only the console and XHDUpdater use them, and they have no per-session copy).
static bool smbus_bridge_allowed(uint8_t cmd)
{
    return !(cmd == I2C_HDMI_COMMAND_READ_CONFIG ||
             (cmd >= I2C_HDMI_COMMAND_READ_RAM && cmd <= I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4) ||
             (cmd >= I2C_HDMI_COMMAND_WRITE_CONFIG && cmd <= I2C_HDMI_COMMAND_WRITE_CONFIG_APPLY) ||
             (cmd >= I2C_HDMI_COMMAND_WRITE_READ_PAGE && cmd <= I2C_HDMI_COMMAND_WRITE_APP_FLASH_MODE));
}

bool smbus_bridge_read(uint8_t cmd, uint8_t *out)
{
    if (!smbus_bridge_allowed(cmd)) return false;
    *out = smbus_read_byte(&s_uart, cmd, 0);
    return true;
}

bool smbus_bridge_write(uint8_t cmd, uint8_t value)
{
    if (!smbus_bridge_allowed(cmd)) return false;
    smbus_write_byte(&s_uart, cmd, value);
    return true;
}

// The slave's hooks (smbus_slave_common.h), from the I2C2 ISR: the console's session, and
// the sniffer records only its traffic -- the UART bridge calls the handlers directly.
void smbus_slave_on_address(void)
{
    // Being addressed at 0x69 is itself proof of SMBus activity -- keep this flag fed even
    // when the passive edge probe is not armed (encoder already self-latched), so the
    // diagnostic-LED grace-period logic that reads smbus_probe_activity_seen() still works.
    smbus_note_probe_activity();
    smbus_note_host_addressed();
}

uint8_t smbus_slave_read(uint8_t cmd, uint8_t prev)
{
    const uint8_t r = smbus_read_byte(&s_smbus, cmd, prev);
    sniff_record(0, cmd, r);
    return r;
}

void smbus_slave_write(uint8_t cmd, uint8_t value)
{
    smbus_write_byte(&s_smbus, cmd, value);
    sniff_record(1, cmd, value);
}

bool video_mode_updated() {
    return video_mode_update_pending;
}

uint32_t get_video_mode_update_sequence() {
    return update_sequence;
}

void ack_video_mode_update(uint32_t seen_sequence) {
    // Only clear pending if nothing newer arrived while the caller was busy
    // applying seen_sequence -- otherwise a command that came in mid-apply
    // would have its pending flag wiped out here without ever being applied.
    if (update_sequence == seen_sequence) {
        video_mode_update_pending = false;
    }
}

const SMBusSettings * getSMBusSettings() {
    return &settings;
}

bool smbus_consume_spd_apply_pending(void) {
    if (s_spd_apply_pending) {
        s_spd_apply_pending = false;
        return true;
    }
    return false;
}

bool consume_color_space_reapply_pending(void) {
    if (color_space_reapply_pending) {
        color_space_reapply_pending = false;
        return true;
    }
    return false;
}

void smbus_telemetry_event(xhd_telemetry_event_t ev)
{
    // All callers are main-loop context; the only concurrent access is the slave ISR
    // reading s_telemetry byte-by-byte for opcode 15. The u8 counters can't tear (single
    // store); mode_change_count is u16, so its read-modify-write is bracketed so the ISR
    // can't sample a half-incremented value across a carry.
    switch (ev)
    {
        case XHD_TLM_EV_MODE_CHANGE:
            __disable_irq();
            if (s_telemetry.mode_change_count != 0xFFFF) s_telemetry.mode_change_count++;
            __enable_irq();
            break;
        case XHD_TLM_EV_PLL_LOSS:
            if (s_telemetry.pll_loss_count != 0xFF) s_telemetry.pll_loss_count++;
            break;
        case XHD_TLM_EV_GATE_CYCLE:
            if (s_telemetry.gate_cycle_count != 0xFF) s_telemetry.gate_cycle_count++;
            break;
        case XHD_TLM_EV_BLIND:
            if (s_telemetry.blind_event_count != 0xFF) s_telemetry.blind_event_count++;
            break;
    }
}

void smbus_telemetry_note_gate_cycle(void)
{
    smbus_telemetry_event(XHD_TLM_EV_GATE_CYCLE);
}

void smbus_telemetry_note_mode_apply(void)
{
    smbus_telemetry_event(XHD_TLM_EV_MODE_CHANGE);
}

void smbus_note_arbiter_decision(uint8_t arb_source, uint8_t arb_target_vic)
{
    s_telemetry.arb_source = arb_source;
    s_telemetry.arb_target_vic = arb_target_vic;
}

void smbus_publish_telemetry(uint32_t pixel_clock_hz, uint8_t detected_input_vic,
                             uint8_t hw_flags, uint8_t probed_encoder)
{
    s_telemetry.pixel_clock_hz     = pixel_clock_hz;
    s_telemetry.detected_input_vic = detected_input_vic;
    s_telemetry.hw_flags           = hw_flags;
    s_telemetry.probed_encoder     = probed_encoder;

    uint32_t up = HAL_GetTick() / 1000u;
    s_telemetry.uptime_s = (up > 0xFFFFu) ? 0xFFFFu : (uint16_t)up;
}

void smbus_publish_video_info(const XhdVideoInfo *vi)
{
    XhdVideoInfo v = *vi;
    v.struct_version = XHD_VIDEO_INFO_VERSION;
    v.struct_size = (uint8_t)sizeof(XhdVideoInfo);
    v.change_count = (uint8_t)s_telemetry.mode_change_count;
    __disable_irq();
    s_video_live = v;
    __enable_irq();
}

uint16_t smbus_video_refresh_mhz(void)
{
    return s_video_live.refresh_mhz;
}

// Drains a pending WRITE_RAM_APPLY flash write -- see that ISR case's own comment for why
// this is deferred here rather than done directly in the ISR. Called from bios_loop()
// (main-loop context) once per iteration, alongside smbus_consume_settings_changes().
void smbus_consume_flash_write(void) {
    __disable_irq();
    bool pending = s_flash_write_pending;
    uint16_t page = s_flash_write_page;
    uint32_t crc = s_flash_write_crc;
    s_flash_write_pending = false;
    __enable_irq();

    if (!pending) {
        return;
    }

    flash_program_page_verified(page, ram_buffer, RAM_BUFFER_SIZE, crc);
}

// Drains the control-suite write requests both sessions stashed (the console's from the ISR,
// the UART bridge's from the main loop). Called from bios_loop() (main-loop context) so register
// writes, flash saves and mode reapplies happen off-ISR. Flags merge; where both sessions set the
// same value, the UART's (the more recent manual action) wins.
void smbus_consume_settings_changes(const xbox_encoder xb_encoder) {
    __disable_irq();
    const uint16_t c_dirty = s_smbus.dirty;
    const uint8_t c_pos = s_smbus.pos_pending;
    const uint8_t c_audio = s_smbus.audio_mute, c_phase = s_smbus.phase;
    const int16_t c_pos_h = s_smbus.pos_h, c_pos_v = s_smbus.pos_v;
    s_smbus.dirty = 0;
    s_smbus.pos_pending = 0;
    __enable_irq();
    // The UART session is only ever touched from the main loop, like this function.
    const uint16_t u_dirty = s_uart.dirty;
    const uint8_t u_pos = s_uart.pos_pending;
    s_uart.dirty = 0;
    s_uart.pos_pending = 0;

    uint16_t dirty = c_dirty | u_dirty;
    const uint8_t audio_mute = (u_dirty & SM_DIRTY_AUDIO) ? s_uart.audio_mute : c_audio;
    uint8_t phase = (u_dirty & SM_DIRTY_PHASE) ? s_uart.phase : c_phase;
    const uint8_t pos = c_pos | u_pos;
    int16_t pos_h = (u_pos & 0x01) ? s_uart.pos_h : c_pos_h;
    int16_t pos_v = (u_pos & 0x02) ? s_uart.pos_v : c_pos_v;

    if (!dirty) {
        return;
    }

    if (dirty & SM_DIRTY_AUDIO) {
        adv7511_set_audio_mute_hold(audio_mute != 0);
    }
    if (dirty & SM_DIRTY_RELOAD) {
        // Saved config back from page 63 (`ld`); a missing/invalid page keeps the live one.
        // The load touches no register: the saved phase goes on with the encoder setup.
        if (user_config_load()) {
            init_adv_encoder_specific(xb_encoder);
            reapply_active_mode(xb_encoder);
        }
        return;
    }

    if (dirty & SM_DIRTY_PHASE) {
        adv7511_update_register(0xBA, 0b11100000, (uint8_t)(phase << 5));
        user_config_get()->enc_cal[user_config_get_encoder_index(xb_encoder)].clock_phase = phase;
    }

    if (dirty & SM_DIRTY_DEFAULTS) {
        // Supersedes any aspect/scan/save requested in the same batch -- defaults resets
        // the whole struct and persists it, then reapplies once.
        user_config_restore_defaults(xb_encoder);
        user_config_save();
        reapply_active_mode(xb_encoder);
        return;
    }

    // Picture position: live register write, no reapply (same as the `hs`/`vs` CLI).
    if (pos & 0x01) {
        xbox_position(xb_encoder, true, &pos_h, NULL, true);
    }
    if (pos & 0x02) {
        xbox_position(xb_encoder, false, &pos_v, NULL, true);
    }
    if (dirty & SM_DIRTY_WVIC) {
        adv7511_apply_wire_vic_mode();
    }
    // scan_mode affects HD too; aspect_ratio is SD-only, so a lone aspect change while an
    // HD mode is live needs no reapply (the setting is stored and applies on the next SD
    // mode).
    bool want_reapply = (dirty & (SM_DIRTY_SCAN | SM_DIRTY_REAPPLY | SM_DIRTY_WVIC)) != 0;
    if (dirty & SM_DIRTY_ASPECT) {
        uint8_t bvic = adv7511_read_active_vic();
        if (!vic_is_hd(bvic)) {
            want_reapply = true;
        }
    }
    if (want_reapply) {
        reapply_active_mode(xb_encoder);
    }
    if (dirty & SM_DIRTY_SAVE) {
        user_config_save();
    }
}
