#include "user_config.h"
#include "flash.h"
#include "adv7511.h"
#include "debug.h"
#include <string.h>

static xhd_user_config g_config;

// Statically initialized to XHD_SPD_DEFAULT so the bootloader, which never runs
// user_config_init(), sends the factory names too.
static xhd_spd_config g_spd = { .name = XHD_SPD_DEFAULT };

static uint32_t checksum_bytes(const void *p, size_t len)
{
    uint32_t sum = 0x12345678;
    const uint8_t *ptr = (const uint8_t*)p;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + ptr[i];
    }
    return sum;
}

static uint32_t calculate_checksum(const xhd_user_config *cfg)
{
    return checksum_bytes(cfg, sizeof(xhd_user_config) - sizeof(uint32_t)); // Exclude checksum field itself
}

const uint8_t *user_config_get_spd(void)
{
    return g_spd.name;
}

bool user_config_set_spd(const uint8_t *name)
{
    for (int i = 0; i < XHD_SPD_NAME_LEN; i++) {
        if (name[i] < 0x20 || name[i] > 0x7E) {
            return false;
        }
    }
    memcpy(g_spd.name, name, XHD_SPD_NAME_LEN);
    return true;
}

const char* get_xbox_encoder_name(uint8_t encoder)
{
    switch (encoder) {
        case ENCODER_CONEXANT:
            return "Conexant (v1.0-1.3)";
        case ENCODER_FOCUS:
            return "Focus (v1.4-1.5)";
        case ENCODER_XCALIBUR:
            // No version suffix, unlike Conexant/Focus above -- those each cover a real
            // range of board revisions, but Xcalibur only ever shipped as v1.6, so the
            // suffix conveyed no information for this one case.
            return "Xcalibur";
        default:
            return "Unknown Encoder";
    }
}

int user_config_get_encoder_index(xbox_encoder xb_encoder)
{
    if (xb_encoder == ENCODER_CONEXANT) return 1;
    if (xb_encoder == ENCODER_FOCUS) return 2;
    return 0; // Default Xcalibur
}

xhd_user_config* user_config_get(void)
{
    return &g_config;
}

// Per-encoder default clock phase, indexed like enc_cal[] (user_config_get_encoder_index()).
static const uint8_t k_default_phase[3] = {
    XHD_DEFAULT_CLOCK_PHASE_XCALIBUR, XHD_DEFAULT_CLOCK_PHASE_CONEXANT, XHD_DEFAULT_CLOCK_PHASE_FOCUS
};

// The page-63 config if its magic and checksum hold, else NULL.
static const xhd_user_config *saved_config(void)
{
    const xhd_user_config *flash_cfg = (const xhd_user_config*)XHD_CONFIG_FLASH_ADDR;
    if (flash_cfg->magic != XHD_CONFIG_MAGIC || flash_cfg->checksum != calculate_checksum(flash_cfg)) {
        return NULL;
    }
    return flash_cfg;
}

bool user_config_peek_active_encoder(xbox_encoder *out_encoder)
{
    const xhd_user_config *flash_cfg = saved_config();
    if (flash_cfg == NULL || flash_cfg->active_encoder > 2) {
        return false;
    }

    // Matches user_config_get_encoder_index(): 0=Xcalibur, 1=Conexant, 2=Focus
    static const xbox_encoder index_to_encoder[3] = { ENCODER_XCALIBUR, ENCODER_CONEXANT, ENCODER_FOCUS };
    *out_encoder = index_to_encoder[flash_cfg->active_encoder];
    return true;
}

void user_config_restore_defaults(xbox_encoder xb_encoder)
{
    int enc_idx = user_config_get_encoder_index(xb_encoder);

    // Full reset whoever asks (boot fallback, or WRITE_CONFIG_ACTION = 2 over SMBus / the
    // UART bridge): color_space/aspect_ratio/scan_mode and all three encoder slots are cleared,
    // not just the active encoder's phase/profiles.
    memset(&g_config, 0, sizeof(xhd_user_config));
    g_config.magic = XHD_CONFIG_MAGIC;
    memcpy(g_spd.name, XHD_SPD_DEFAULT, XHD_SPD_NAME_LEN);
    g_config.color_space = COLOR_SPACE_AUTO;
    g_config.aspect_ratio = ASPECT_RATIO_AUTO;
    g_config.scan_mode = SCAN_AUTO;

    // profile[] arrays are already all-zero from the memset above -- no per-profile
    // hs/vs calibration by default.
    for (int i = 0; i < 3; i++) {
        g_config.enc_cal[i].clock_phase = k_default_phase[i];
    }

    // Push the reset colour space onto the live chip too -- user_config_save() re-samples
    // the live colour space into g_config just before writing flash, so without this a
    // save() right after a defaults restore would put the old forced value back into Page 63.
    adv7511_set_color_space(COLOR_SPACE_AUTO);

    // enc_idx defaults to slot 0 (Xcalibur) for array-indexing when xb_encoder is
    // ENCODER_UNKNOWN, but active_encoder itself must stay 0xFF ("not yet confirmed") in
    // that case -- committing slot 0 here would make an unconfirmed encoder look identical
    // to a confirmed Xcalibur and defeat the mismatch check that self-latches the real
    // encoder on first contact. user_config_load() clamps 0xFF back to 0 on the next boot.
    g_config.active_encoder = (xb_encoder == ENCODER_UNKNOWN) ? 0xFF : (uint8_t)enc_idx;
    g_config.checksum = calculate_checksum(&g_config);
}

bool user_config_load(void)
{
    const xhd_user_config *flash_cfg = saved_config();
    if (flash_cfg == NULL) {
        return false;
    }

    memcpy(&g_config, flash_cfg, sizeof(xhd_user_config));

    // Validate fields
    if (g_config.color_space > COLOR_SPACE_MAX) {
        g_config.color_space = COLOR_SPACE_AUTO;
    }
    if (g_config.aspect_ratio > ASPECT_RATIO_4_3) {
        g_config.aspect_ratio = ASPECT_RATIO_AUTO;
    }
    if (g_config.wire_vic_mode > WIRE_VIC_AUTO) {
        g_config.wire_vic_mode = WIRE_VIC_HONEST;
    }
    if (g_config.scan_mode > SCAN_PC) {
        g_config.scan_mode = SCAN_AUTO;
    }
    if (g_config.active_encoder > 2) {
        g_config.active_encoder = 0;
    }
    for (int i = 0; i < 3; i++) {
        if (g_config.enc_cal[i].clock_phase > 7) {
            g_config.enc_cal[i].clock_phase = k_default_phase[i];
        }
    }

    // The colour space is firmware state; the clock phase reaches the chip through
    // init_adv_encoder_specific() (chip setup, encoder changes, the `ld` reload) -- at boot
    // this runs before the chip is configured or may be touched.
    adv7511_set_color_space((adv_color_space)g_config.color_space);

    return true;
}

#ifdef BUILD_APPLICATION

bool user_config_save(void)
{
    g_config.magic = XHD_CONFIG_MAGIC;
    g_config.color_space = (uint8_t)adv7511_get_color_space();
    g_config.checksum = calculate_checksum(&g_config);
    g_spd.checksum = checksum_bytes(g_spd.name, XHD_SPD_NAME_LEN);

    // One contiguous image: xhd_user_config exactly where it always was, SPD block after it
    // (see xhd_spd_config's comment in user_config.h).
    #pragma pack(push, 1)
    struct { xhd_user_config cfg; xhd_spd_config spd; } page;
    #pragma pack(pop)
    page.cfg = g_config;
    page.spd = g_spd;

    // Already in flash as-is: skip the erase. The F030's pages are rated for 1k cycles,
    // and a host that saves on every exit would otherwise wear page 63 for nothing.
    if (memcmp((const void *)XHD_CONFIG_FLASH_ADDR, &page, sizeof(page)) == 0) {
        return true;
    }

    // Page 63 also holds the bootloader's app-invalid flag in its last 2 bytes
    // (APP_INVALID_FLAG_ADDRESS) -- preserve it across this erase so a config
    // save can't silently clear it out from under an in-progress firmware update.
    uint16_t invalid_flag = *(volatile uint16_t *)APP_INVALID_FLAG_ADDRESS;

    if (!flash_erase_page(XHD_CONFIG_FLASH_PAGE) ||
        !flash_write_page(XHD_CONFIG_FLASH_PAGE, (uint8_t*)&page, sizeof(page))) {
        debug_log("[CONFIG] save failed\r\n");
        return false;
    }

    if (invalid_flag == APP_INVALID_FLAG) {
        flash_set_flag();
    }

    // Silent on success; the self-latch logs its own line.
    return true;
}

void user_config_init(xbox_encoder xb_encoder)
{
    if (user_config_load()) {
        debug_log("[CONFIG] loaded (phase %u, %s)\r\n",
                  g_config.enc_cal[g_config.active_encoder].clock_phase, get_xbox_encoder_name(xb_encoder));
    } else {
        user_config_restore_defaults(xb_encoder);
        debug_log("[CONFIG] defaults (none saved)\r\n");
    }

    // SPD block: independent of xhd_user_config's own validity. Absent (erased 0xFF, or
    // a page last written by older firmware), bad checksum or non-printable -> keep the
    // defaults already in g_spd.
    const xhd_spd_config *flash_spd =
        (const xhd_spd_config *)(XHD_CONFIG_FLASH_ADDR + sizeof(xhd_user_config));
    if (flash_spd->checksum == checksum_bytes(flash_spd->name, XHD_SPD_NAME_LEN)) {
        user_config_set_spd(flash_spd->name);
    }
}

// Self-latch: called when a live SMBus mode-apply packet (or the bus probe) reports an
// encoder that doesn't match what's currently active, so the correction survives a
// reboot. One binary serves all 3 encoders, so this fires on the normal boot path for
// 2 of 3 boards. Both callers have just run init_adv_encoder_specific(), which put this
// encoder's phase on the chip.
void user_config_set_active_encoder(xbox_encoder xb_encoder)
{
    g_config.active_encoder = (uint8_t)user_config_get_encoder_index(xb_encoder);
    if (user_config_save()) {
        debug_log("[CONFIG] self-latched %s\r\n", get_xbox_encoder_name(xb_encoder));
    }
}

#else

void user_config_init(xbox_encoder xb_encoder)
{
    (void)xb_encoder;
}

#endif // BUILD_APPLICATION
