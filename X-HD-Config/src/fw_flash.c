// SPDX-License-Identifier: GPL-3.0-only
// Firmware update over SMBus. See fw_flash.h. The X-HD protocol is doc/SMBUS.md section 10;
// the HD+ conversion follows XHDUpdater (Team-Resurgent,
// github.com/Team-Resurgent/XHDUpdater: HDPlusHelper.cpp and Main.cpp HDPlusProcess()), timings included -- the HD+ bootloader is
// MakeMHz's and undocumented, so its waits are kept as XHDUpdater has them.
#include "fw_flash.h"
#include "defines.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define XHD_ADDR            XHD_SMBUS_ADDR

// HD+ (stock MakeMHz firmware) opcodes, from XHDUpdater's HDPlusHelper.h.
#define HDP_READ_READY1     0x36
#define HDP_READ_READY2     0x37
#define HDP_READ_FLASH_OK   0x3C
#define HDP_READ_VERSION1   0x57
#define HDP_READ_MODE       0x20
#define HDP_WRITE_SET_MODE  0x29
#define HDP_WRITE_SET_PAGE  0x2E
#define HDP_WRITE_SET_FLASH 0x2F
#define HDP_WRITE_PAGE_CRC1 0x38
#define HDP_MODE_APP        0x00
#define HDP_MODE_BOOT       0x40
#define HDP_MODE_EN_FLASH   0x02
#define HDP_MODE_EN_APP     0x60
#define HDP_APP_OFFSET      0x2800  // the HD+ application starts at page 10 (0x08002800)
#define HDP_VECTORS         0xC0    // vector table copied to the HD+ application start

// Waits, in ms. "Safe" is XHDUpdater's sequence, for firmware without READ_FLASH_STATUS;
// with it, the status is polled instead of the one-second wait. A page that fails twice on
// the fast timings drops the rest of the run to the safe ones.
#define MODE_SWITCH_TIMEOUT_MS  10000
#define APP_START_TIMEOUT_MS    15000
#define MODE_POLL_MS            50
#define SAFE_BYTE_GAP_MS        1
#define SAFE_APPLY_WAIT_MS      1000
#define SAFE_READ_PAGE_WAIT_MS  250
#define FAST_READ_PAGE_WAIT_MS  20
#define STATUS_POLL_MS          5
// Before the first status read. The bootloader erases and writes the page inside its I2C ISR
// once the STOP arrives, so it never reports BUSY: a read that lands before that answers the
// previous status (IDLE on the first page -- the likely reason page 20 failed twice on hardware), and one
// during it is clock-stretched past the host's timeout. STM32F030 page erase is up to 40 ms
// and 512 half-word writes up to ~31 ms (datasheet maxima).
#define STATUS_FIRST_WAIT_MS    80
#define STATUS_TIMEOUT_MS       2000
#define FLAG_CLEAR_WAIT_MS      200
#define PAGE_TRIES              4
#define HDP_READY_TIMEOUT_MS    30000

// Cortex-M0 vector checks: initial SP inside the 8 KB SRAM, reset handler a Thumb address
// inside the image's own region.
#define SRAM_START   0x20000000u
#define SRAM_END     0x20002000u
#define FLASH_BASE_  0x08000000u
#define APP_BASE     (FLASH_BASE_ + FW_BL_PAGES * FW_PAGE_SIZE)
#define APP_END      (FLASH_BASE_ + FW_MAX_IMAGE)
// src/shared/app_header.h
#define APP_HDR_OFFSET 0xC0u
#define APP_HDR_MAGIC  0x31485841u

typedef struct {
    bool     status;         // poll READ_FLASH_STATUS instead of waiting
    uint32_t byte_gap;
    uint32_t read_page_wait;
} timing_t;

static uint8_t s_page[FW_PAGE_SIZE];

// ---------------------------------------------------------------------------- CRC / image

// `crc` is a previous result (0 to start), as zlib's crc32().
static uint32_t fw_crc32_continue(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    static uint32_t table[256];
    if (table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            table[i] = c;
        }
    }
    crc ^= 0xFFFFFFFFu;
    while (len--) crc = (crc >> 8) ^ table[(crc ^ *buf++) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

uint32_t fw_crc32(const uint8_t *buf, uint32_t len)
{
    return fw_crc32_continue(0, buf, len);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool vectors_ok(const uint8_t *v, uint32_t lo, uint32_t hi)
{
    uint32_t sp = rd32(v), entry = rd32(v + 4);
    return sp > SRAM_START && sp <= SRAM_END && (sp & 3) == 0 &&
           entry >= lo && entry < hi && (entry & 1);
}

// The application's about block (src/application/smbus_i2c.c s_about) as its .data initial
// value: version 1, size 60, 14 zero bytes (chip ID/rev, MCU UID), then the git hash and the
// build date, NUL-padded.
static bool find_about(const fw_image_t *img, fw_image_t *out)
{
    const uint32_t start = FW_BL_PAGES * FW_PAGE_SIZE;
    for (uint32_t o = start; o + 60 <= img->size; o++) {
        const uint8_t *p = img->data + o;
        if (p[0] != 1 || p[1] != 60) continue;
        bool zeros = true;
        for (int i = 2; i < 16 && zeros; i++) zeros = p[i] == 0;
        if (!zeros || p[16] < 0x21 || p[16] > 0x7E || p[40] != '2') continue;
        if (memchr(p + 16, 0, 24) == NULL || memchr(p + 40, 0, 20) == NULL) continue;
        memcpy(out->git, p + 16, 24);
        memcpy(out->built, p + 40, 20);
        return true;
    }
    return false;
}

const char *fw_image_check(const uint8_t *data, uint32_t size, fw_image_t *out)
{
    memset(out, 0, sizeof(*out));
    out->data = data;
    out->size = size;
    if (size <= FW_BL_PAGES * FW_PAGE_SIZE + 8) {
        return "Too small for a bootloader + application image";
    }
    if (size > FW_MAX_IMAGE) {
        return "Too large: it would overwrite the saved settings (page 63)";
    }
    if (!vectors_ok(data, FLASH_BASE_, APP_BASE)) {
        return "No X-HD bootloader at the start of the file";
    }
    if (!vectors_ok(data + FW_BL_PAGES * FW_PAGE_SIZE, APP_BASE, APP_END)) {
        return "No X-HD application at 20 KB into the file";
    }
    out->app_pages = (size - FW_BL_PAGES * FW_PAGE_SIZE + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;
    // The image header (src/shared/app_header.h), when the build stamped one: a file damaged
    // on the way to the Xbox is refused here, before any page is written.
    const uint8_t *app = data + FW_BL_PAGES * FW_PAGE_SIZE;
    const uint32_t app_size = size - FW_BL_PAGES * FW_PAGE_SIZE;
    if (app_size >= APP_HDR_OFFSET + 12 && rd32(app + APP_HDR_OFFSET) == APP_HDR_MAGIC) {
        const uint32_t len = rd32(app + APP_HDR_OFFSET + 4);
        if (len > app_size || len < APP_HDR_OFFSET + 12 || (len & 3)) {
            return "Damaged file: its image header is wrong";
        }
        uint32_t crc = fw_crc32_continue(0, app, APP_HDR_OFFSET + 8);
        crc = fw_crc32_continue(crc, app + APP_HDR_OFFSET + 12, len - APP_HDR_OFFSET - 12);
        if (crc != rd32(app + APP_HDR_OFFSET + 8)) {
            return "Damaged file: its CRC doesn't match";
        }
        out->has_header = true;
    }
    out->has_about = find_about(out, out);
    return NULL;
}

void fw_image_page(const fw_image_t *img, uint32_t page, uint8_t out[FW_PAGE_SIZE])
{
    uint32_t off = page * FW_PAGE_SIZE;
    memset(out, 0xFF, FW_PAGE_SIZE);
    if (off < img->size) {
        uint32_t n = img->size - off;
        memcpy(out, img->data + off, n < FW_PAGE_SIZE ? n : FW_PAGE_SIZE);
    }
}

bool fw_image_has_u32(const fw_image_t *img, uint32_t value)
{
    for (uint32_t o = FW_BL_PAGES * FW_PAGE_SIZE; o + 4 <= img->size; o++) {
        if (rd32(img->data + o) == value) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- bus helpers

static void logf_(const fw_io_t *io, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io->log(buf);
}

static bool xhd_mode(const fw_io_t *io, uint8_t *mode)
{
    return io->read(XHD_ADDR, XHD_OP_READ_MODE, mode);
}

static bool wait_mode(const fw_io_t *io, uint8_t want, uint32_t timeout_ms)
{
    const uint32_t start = io->ticks_ms();
    for (;;) {
        uint8_t m = 0;
        if (xhd_mode(io, &m) && m == want) return true;
        if (io->ticks_ms() - start >= timeout_ms) return false;
        io->sleep_ms(MODE_POLL_MS);
    }
}

static bool has_status_op(const fw_io_t *io)
{
    uint8_t v = 0xFF;
    return io->read(XHD_ADDR, XHD_OP_READ_FLASH_STATUS, &v) && v <= XHD_FLASH_FAILED;
}

static void timing_init(const fw_io_t *io, timing_t *t)
{
    t->status = has_status_op(io);
    t->byte_gap = t->status ? 0 : SAFE_BYTE_GAP_MS;
    t->read_page_wait = t->status ? FAST_READ_PAGE_WAIT_MS : SAFE_READ_PAGE_WAIT_MS;
}

static void timing_safe(const fw_io_t *io, timing_t *t)
{
    if (t->status || t->byte_gap != SAFE_BYTE_GAP_MS) {
        io->log("Switching to legacy mode (slower)...");
    }
    t->status = false;
    t->byte_gap = SAFE_BYTE_GAP_MS;
    t->read_page_wait = SAFE_READ_PAGE_WAIT_MS;
}

// WRITE_READ_PAGE, then the page's CRC (opcodes 7-10, MSB first). False on a bus error.
static bool board_page_crc(const fw_io_t *io, uint8_t page, uint32_t wait_ms, uint32_t *crc)
{
    if (!io->write(XHD_ADDR, XHD_OP_WRITE_READ_PAGE, page)) return false;
    io->sleep_ms(wait_ms);
    uint32_t c = 0;
    for (uint8_t op = XHD_OP_READ_RAM_PAGE_CRC1; op <= XHD_OP_READ_RAM_PAGE_CRC4; op++) {
        uint8_t b;
        if (!io->read(XHD_ADDR, op, &b)) return false;
        c = (c << 8) | b;
    }
    *crc = c;
    return true;
}

static bool page_matches(const fw_io_t *io, uint8_t page, const uint8_t *buf, uint32_t wait_ms)
{
    uint32_t crc;
    return board_page_crc(io, page, wait_ms, &crc) && crc == fw_crc32(buf, FW_PAGE_SIZE);
}

// After WRITE_RAM_APPLY: the status once it leaves BUSY (or a fixed wait without the opcode).
static bool apply_wait(const fw_io_t *io, const timing_t *t)
{
    if (!t->status) {
        io->sleep_ms(SAFE_APPLY_WAIT_MS);
        return true;
    }
    io->sleep_ms(STATUS_FIRST_WAIT_MS);
    const uint32_t start = io->ticks_ms();
    for (;;) {
        uint8_t st = XHD_FLASH_BUSY;
        if (io->read(XHD_ADDR, XHD_OP_READ_FLASH_STATUS, &st) && st != XHD_FLASH_BUSY) {
            return st == XHD_FLASH_OK;
        }
        if (io->ticks_ms() - start >= STATUS_TIMEOUT_MS) return false;
        io->sleep_ms(STATUS_POLL_MS);
    }
}

// One page: buffer, apply, then read it back -- up to PAGE_TRIES times.
static bool program_page(const fw_io_t *io, uint8_t page, const uint8_t *buf, timing_t *t)
{
    for (int attempt = 1; attempt <= PAGE_TRIES; attempt++) {
        bool sent = io->write(XHD_ADDR, XHD_OP_WRITE_RAM_BANK, 0);
        for (int i = 0; i < FW_PAGE_SIZE && sent; i++) {
            sent = io->write(XHD_ADDR, XHD_OP_WRITE_RAM, buf[i]);
            if (t->byte_gap) io->sleep_ms(t->byte_gap);
        }
        if (sent) sent = io->write(XHD_ADDR, XHD_OP_WRITE_RAM_APPLY, page);
        bool written = sent && apply_wait(io, t);
        if (written && page_matches(io, page, buf, t->read_page_wait)) {
            return true;
        }
        logf_(io, "Page %u: %s (try %d of %d)", (unsigned)page,
              !sent ? "SMBus error" : !written ? "write failed" : "read-back differs",
              attempt, PAGE_TRIES);
        if (attempt == 2) timing_safe(io, t);
    }
    return false;
}

// ---------------------------------------------------------------------------- detection

bool fw_detect(const fw_io_t *io, fw_board_t *out)
{
    memset(out, 0, sizeof(*out));
    bool ok = true, all_ff = true;
    for (int i = 0; i < 4 && ok; i++) {
        ok = io->read(XHD_ADDR, (uint8_t)(XHD_OP_READ_VERSION1 + i), &out->ver[i]);
        all_ff = all_ff && out->ver[i] == 0xFF;
    }
    // A bootloader that answers FF FF FF FF for the version is still found by its mode.
    if (ok && xhd_mode(io, &out->mode) &&
        ((!all_ff && out->mode == XHD_MODE_APPLICATION) || out->mode == XHD_MODE_BOOTLOADER)) {
        out->kind = out->mode == XHD_MODE_APPLICATION ? FW_BOARD_XHD_APP : FW_BOARD_XHD_BOOT;
        out->status_op = has_status_op(io);
        return true;
    }

    memset(out, 0, sizeof(*out));
    ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        ok = io->read(FW_HDPLUS_ADDR, (uint8_t)(HDP_READ_VERSION1 + i), &out->ver[i]);
    }
    if (ok && io->read(FW_HDPLUS_ADDR, HDP_READ_MODE, &out->mode)) {
        out->kind = FW_BOARD_HDPLUS;
        return true;
    }
    memset(out, 0, sizeof(*out));
    return false;
}

bool fw_read_build(const fw_io_t *io, char git[24], char built[20])
{
    uint8_t ab[60];
    if (!io->write(XHD_ADDR, XHD_OP_WRITE_ABOUT_INDEX, 0)) return false;
    for (int i = 0; i < 60; i++) {
        if (!io->read(XHD_ADDR, XHD_OP_READ_ABOUT, &ab[i])) return false;
    }
    if (ab[0] != 1 || ab[1] != 60) return false;
    memcpy(git, ab + 16, 24);
    memcpy(built, ab + 40, 20);
    git[23] = built[19] = '\0';
    return true;
}

// ---------------------------------------------------------------------------- X-HD jobs

bool fw_compare(const fw_io_t *io, const fw_image_t *img, fw_compare_t *out)
{
    out->app_diff = out->bl_diff = 0;
    const uint32_t last = FW_BL_PAGES + img->app_pages;
    for (uint32_t p = 0; p < last; p++) {
        fw_image_page(img, p, s_page);
        // Read-only: a CRC lost to a short wait only shows as a difference.
        if (!page_matches(io, (uint8_t)p, s_page, FAST_READ_PAGE_WAIT_MS)) {
            if (p < FW_BL_PAGES) out->bl_diff++; else out->app_diff++;
        }
        io->progress(p + 1, last);
    }
    return true;
}

bool fw_read_board(const fw_io_t *io, uint8_t *out)
{
    const uint32_t pages = FW_BACKUP_SIZE / FW_PAGE_SIZE;
    for (uint32_t p = 0; p < pages; p++) {
        uint8_t *dst = out + p * FW_PAGE_SIZE;
        bool ok = false;
        for (int attempt = 0; attempt < PAGE_TRIES && !ok; attempt++) {
            uint32_t crc;
            if (!board_page_crc(io, (uint8_t)p, FAST_READ_PAGE_WAIT_MS * (attempt + 1), &crc) ||
                !io->write(XHD_ADDR, XHD_OP_WRITE_RAM_BANK, 0)) {
                continue;
            }
            ok = true;
            for (int i = 0; i < FW_PAGE_SIZE && ok; i++) {
                ok = io->read(XHD_ADDR, XHD_OP_READ_RAM, &dst[i]);
            }
            ok = ok && fw_crc32(dst, FW_PAGE_SIZE) == crc;
        }
        if (!ok) {
            logf_(io, "Page %u could not be read back", (unsigned)p);
            return false;
        }
        io->progress(p + 1, pages);
    }
    return true;
}

// The app-invalid flag is the last halfword of page 63; erased (FF FF) when clear.
static bool flag_clear(const fw_io_t *io, uint32_t wait_ms)
{
    uint32_t crc;
    uint8_t lo = 0, hi = 0;
    return board_page_crc(io, FW_CONFIG_PAGE, wait_ms, &crc) &&
           io->write(XHD_ADDR, XHD_OP_WRITE_RAM_BANK, 3) &&
           io->write(XHD_ADDR, XHD_OP_WRITE_RAM_INDEX, 254) &&
           io->read(XHD_ADDR, XHD_OP_READ_RAM, &lo) &&
           io->read(XHD_ADDR, XHD_OP_READ_RAM, &hi) &&
           lo == 0xFF && hi == 0xFF;
}

bool fw_update_app(const fw_io_t *io, const fw_image_t *img)
{
    uint8_t mode = 0;
    if (!xhd_mode(io, &mode) || (mode != XHD_MODE_APPLICATION && mode != XHD_MODE_BOOTLOADER)) {
        io->log("No board answers");
        return false;
    }
    if (mode == XHD_MODE_APPLICATION) {
        io->log("Restarting the board in update mode...");
        io->write(XHD_ADDR, XHD_OP_WRITE_SET_MODE, XHD_MODE_BOOTLOADER);
        if (!wait_mode(io, XHD_MODE_BOOTLOADER, MODE_SWITCH_TIMEOUT_MS)) {
            io->log("The board did not enter update mode; nothing was written");
            return false;
        }
    } else {
        io->log("Board already in update mode: resuming");
    }

    timing_t t;
    timing_init(io, &t);
    if (!t.status) io->log("Legacy firmware: using legacy mode (slower)");

    // From here until the flag is cleared, any power loss leaves the board in update mode.
    if (!io->write(XHD_ADDR, XHD_OP_WRITE_RAM_BANK, 0) ||
        !io->write(XHD_ADDR, XHD_OP_WRITE_APP_FLASH_MODE, 1)) {
        io->log("SMBus error starting the update");
        return false;
    }
    io->sleep_ms(FLAG_CLEAR_WAIT_MS);

    int written = 0;
    for (uint32_t i = 0; i < img->app_pages; i++) {
        const uint8_t page = (uint8_t)(FW_BL_PAGES + i);
        fw_image_page(img, page, s_page);
        if (!page_matches(io, page, s_page, t.read_page_wait)) {
            if (!program_page(io, page, s_page, &t)) {
                logf_(io, "Page %u would not write. The board stays in update mode:", (unsigned)page);
                io->log("choose Finish the update to try again");
                return false;
            }
            written++;
        }
        io->progress(i + 1, img->app_pages);
    }
    io->log(written ? "Firmware written" : "Firmware already on the board");

    bool cleared = false;
    for (int attempt = 0; attempt < 2 && !cleared; attempt++) {
        io->write(XHD_ADDR, XHD_OP_WRITE_APP_FLASH_MODE, 0);
        io->sleep_ms(FLAG_CLEAR_WAIT_MS);
        cleared = flag_clear(io, t.read_page_wait);
    }
    if (!cleared) {
        // Not fatal on its own: whether the application starts below is the real test.
        io->log("Could not read back the update flag; trying to start the application");
    }

    io->log("Starting the new application...");
    io->write(XHD_ADDR, XHD_OP_WRITE_SET_MODE, XHD_MODE_APPLICATION);
    if (!wait_mode(io, XHD_MODE_APPLICATION, APP_START_TIMEOUT_MS)) {
        io->log("The new application did not start. Choose Finish the update to try again");
        return false;
    }
    return true;
}

// Set when a bootloader page would not write: the board runs on, but won't start again after
// a power-off until the bootloader is written.
static bool s_bl_incomplete;
bool fw_bootloader_incomplete(void) { return s_bl_incomplete; }

bool fw_update_bootloader(const fw_io_t *io, const fw_image_t *img)
{
    uint8_t mode = 0;
    if (!xhd_mode(io, &mode) || mode != XHD_MODE_APPLICATION) {
        io->log("The board's firmware must be running to write the bootloader");
        return false;
    }
    timing_t t;
    timing_init(io, &t);
    int written = 0;
    for (uint8_t page = 0; page < FW_BL_PAGES; page++) {
        fw_image_page(img, page, s_page);
        if (!page_matches(io, page, s_page, t.read_page_wait)) {
            if (!program_page(io, page, s_page, &t)) {
                s_bl_incomplete = true;
                logf_(io, "Bootloader page %u failed. Don't power off: choose Retry the update.", (unsigned)page);
                io->log("If it keeps failing, recovering the board needs an ST-Link.");
                return false;
            }
            written++;
        }
        io->progress(page + 1, FW_BL_PAGES);
    }
    s_bl_incomplete = false;
    if (written) io->log("Bootloader updated");
    return true;
}

bool fw_update_firmware(const fw_io_t *io, const fw_image_t *img, bool *app_written)
{
    *app_written = false;
    uint8_t mode = 0;
    const bool running = xhd_mode(io, &mode) && mode == XHD_MODE_APPLICATION;
    // The bootloader first, written by the running application: a new application reads its
    // mode tables from the bootloader (src/shared/xbox_timing_rows.h), so they must be there
    // when it starts -- and until then the old one shows the picture from its own. Only a
    // board already in update mode (no application to write it) gets it after.
    if (running) {
        if (!fw_update_bootloader(io, img)) return false;
    }
    // On a running application whose pages all match, the application stage would only send
    // the board through update mode and back.
    bool app_matches = running;
    for (uint32_t i = 0; i < img->app_pages && app_matches; i++) {
        fw_image_page(img, FW_BL_PAGES + i, s_page);
        app_matches = page_matches(io, (uint8_t)(FW_BL_PAGES + i), s_page, FAST_READ_PAGE_WAIT_MS);
    }
    if (!app_matches) {
        if (!fw_update_app(io, img)) return false;
        *app_written = true;
    }
    if (!running) {
        return fw_update_bootloader(io, img);
    }
    return true;
}

// ---------------------------------------------------------------------------- HD+

// HD+ busy: its READY opcodes NACK until a page is done.
static bool hdp_wait_ready(const fw_io_t *io)
{
    const uint32_t start = io->ticks_ms();
    uint8_t v;
    while (!(io->read(FW_HDPLUS_ADDR, HDP_READ_READY2, &v) && io->read(FW_HDPLUS_ADDR, HDP_READ_READY1, &v))) {
        if (io->ticks_ms() - start >= HDP_READY_TIMEOUT_MS) return false;
        io->sleep_ms(MODE_POLL_MS);
    }
    return true;
}

// The X-HD application laid out for the HD+ bootloader: its vector table at the HD+
// application start (page 10), the application itself at page 20 where it was linked.
static uint8_t s_hdp_image[HDP_APP_OFFSET + FW_MAX_IMAGE - FW_BL_PAGES * FW_PAGE_SIZE];

bool fw_convert_hdplus(const fw_io_t *io, const fw_image_t *img)
{
    uint8_t mode = 0;
    if (!io->read(FW_HDPLUS_ADDR, HDP_READ_MODE, &mode)) {
        io->log("No HD+ answers");
        return false;
    }
    if (mode == HDP_MODE_APP) {
        io->log("Restarting the HD+ in update mode...");
        io->sleep_ms(1000);
        io->write(FW_HDPLUS_ADDR, HDP_WRITE_SET_MODE, HDP_MODE_BOOT | HDP_MODE_EN_FLASH);
        io->sleep_ms(1000);
    }

    const uint32_t app_size = img->size - FW_BL_PAGES * FW_PAGE_SIZE;
    const uint32_t size = HDP_APP_OFFSET + app_size;
    memset(s_hdp_image, 0xFF, sizeof(s_hdp_image));
    memcpy(s_hdp_image, img->data + FW_BL_PAGES * FW_PAGE_SIZE, HDP_VECTORS);
    memcpy(s_hdp_image + HDP_APP_OFFSET, img->data + FW_BL_PAGES * FW_PAGE_SIZE, app_size);
    const uint32_t pages = (size + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        memset(s_page, 0xFF, FW_PAGE_SIZE);
        uint32_t n = size - i * FW_PAGE_SIZE;
        memcpy(s_page, s_hdp_image + i * FW_PAGE_SIZE, n < FW_PAGE_SIZE ? n : FW_PAGE_SIZE);
        const uint32_t crc = fw_crc32(s_page, FW_PAGE_SIZE);

        bool ok = false;
        for (int attempt = 1; attempt <= PAGE_TRIES && !ok; attempt++) {
            io->sleep_ms(2000);
            if (!hdp_wait_ready(io)) {
                io->log("The HD+ stopped answering");
                break;
            }
            io->sleep_ms(1000);
            for (int b = 0; b < 4; b++) {   // LSB first
                io->write(FW_HDPLUS_ADDR, (uint8_t)(HDP_WRITE_PAGE_CRC1 + b), (uint8_t)(crc >> (8 * b)));
            }
            io->sleep_ms(1000);
            io->write(FW_HDPLUS_ADDR, HDP_WRITE_SET_PAGE, (uint8_t)i);
            for (int b = 0; b < FW_PAGE_SIZE; b++) {
                io->write(FW_HDPLUS_ADDR, HDP_WRITE_SET_FLASH, s_page[b]);
            }
            io->sleep_ms(1000);
            uint8_t flash_ok = 0xFF;
            if (hdp_wait_ready(io)) {
                io->sleep_ms(1000);
                ok = io->read(FW_HDPLUS_ADDR, HDP_READ_FLASH_OK, &flash_ok) && flash_ok == 0;
            }
            if (!ok) {
                logf_(io, "HD+ page %u: write failed (try %d of %d)", (unsigned)(i + 10), attempt, PAGE_TRIES);
            }
        }
        if (!ok) {
            io->log("The conversion failed. Recovering the board needs an ST-Link.");
            return false;
        }
        io->progress(i + 1, pages);
    }

    io->log("Starting the X-HD application...");
    io->sleep_ms(1000);
    io->write(FW_HDPLUS_ADDR, HDP_WRITE_SET_MODE, HDP_MODE_APP | HDP_MODE_EN_APP);
    io->sleep_ms(1000);
    if (!wait_mode(io, XHD_MODE_APPLICATION, APP_START_TIMEOUT_MS)) {
        io->log("The X-HD application did not start. Recovering the board needs an ST-Link.");
        return false;
    }

    io->log("Writing the X-HD bootloader...");
    return fw_update_bootloader(io, img);
}
