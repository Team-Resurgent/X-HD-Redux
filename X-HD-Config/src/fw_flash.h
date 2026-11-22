// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Firmware update over SMBus: image checks and the board-side sequences (X-HD update, X-HD
// bootloader update, HD+ -> X-HD conversion, read-back). Blocking, and free of Xbox headers:
// the bus, the clock and the messages go through fw_io_t, so the host tests
// run the same code against a simulated board. fw_update.c runs it on a worker thread.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_PAGE_SIZE      1024
#define FW_BL_PAGES       20                              // bootloader: pages 0-19
#define FW_CONFIG_PAGE    63                              // saved settings + app-invalid flag
#define FW_MAX_IMAGE      (FW_CONFIG_PAGE * FW_PAGE_SIZE) // an image must stop short of page 63
#define FW_BACKUP_SIZE    FW_MAX_IMAGE                    // pages 0-62: itself a flashable image

#define FW_HDPLUS_ADDR    0x88    // MakeMHz HD+ (7-bit 0x44), 8-bit as the kernel takes it

typedef struct {
    bool     (*read)(uint8_t addr, uint8_t cmd, uint8_t *val);   // false = NACK / bus error
    bool     (*write)(uint8_t addr, uint8_t cmd, uint8_t val);
    void     (*sleep_ms)(uint32_t ms);
    uint32_t (*ticks_ms)(void);
    void     (*progress)(uint32_t done, uint32_t total);
    void     (*log)(const char *msg);
} fw_io_t;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t app_pages;   // application pages in the image, from page 20
    bool     has_about;   // the application's about block was found: git/built are valid
    bool     has_header;  // the build stamped an image header (length + CRC), and it matches
    char     git[24];
    char     built[20];
} fw_image_t;

typedef enum {
    FW_BOARD_NONE = 0,
    FW_BOARD_XHD_APP,     // X-HD running its application
    FW_BOARD_XHD_BOOT,    // X-HD in its bootloader (an update was started or interrupted)
    FW_BOARD_HDPLUS,      // stock MakeMHz HD+ firmware
} fw_board_kind_t;

typedef struct {
    fw_board_kind_t kind;
    uint8_t ver[4];       // X-HD: opcodes 1-4. HD+: three version bytes
    uint8_t mode;         // the raw READ_MODE answer
    bool    status_op;    // X-HD answers READ_FLASH_STATUS (26)
} fw_board_t;

typedef struct {
    int app_diff;         // image application pages that differ from the board's
    int bl_diff;          // bootloader pages (0-19) that differ
} fw_compare_t;

// NULL if `data` is a combined X-HD image (bootloader + application, as firmware.bin), else
// why not. Fills `out`; `data` must stay valid while `out` is used.
const char *fw_image_check(const uint8_t *data, uint32_t size, fw_image_t *out);
// Page `page` (0-62) of the image, padded with 0xFF past its end.
void fw_image_page(const fw_image_t *img, uint32_t page, uint8_t out[FW_PAGE_SIZE]);
// True if the image contains `value` (little-endian) in its application part.
bool fw_image_has_u32(const fw_image_t *img, uint32_t value);
uint32_t fw_crc32(const uint8_t *buf, uint32_t len);

bool fw_detect(const fw_io_t *io, fw_board_t *out);
// CRC of every page the image covers, against the board's. X-HD boards only.
bool fw_compare(const fw_io_t *io, const fw_image_t *img, fw_compare_t *out);
// Pages 0-62 into `out` (FW_BACKUP_SIZE bytes), each checked against the board's CRC.
bool fw_read_board(const fw_io_t *io, uint8_t *out);
// Application update through the bootloader; writes only the pages that differ. From the
// application or from the bootloader (resuming an interrupted update); ends in the new
// application.
bool fw_update_app(const fw_io_t *io, const fw_image_t *img);
// Bootloader pages that differ, written by the running X-HD application.
bool fw_update_bootloader(const fw_io_t *io, const fw_image_t *img);
// The whole image, as one update: the bootloader pages that differ, written by the running
// application, then the application (skipped when all its pages match) -- the new
// application reads its mode tables from the bootloader. A board in update mode gets the
// application first, then the bootloader. Run again after a failure: it redoes only what
// still differs. *app_written: the application was rewritten, so the board restarted.
bool fw_update_firmware(const fw_io_t *io, const fw_image_t *img, bool *app_written);
// A bootloader page would not write (since the last bootloader update that finished): the
// board runs, but won't start after a power-off until the bootloader is written.
bool fw_bootloader_incomplete(void);
// Stock HD+ -> X-HD: the application through the HD+ bootloader, then the X-HD bootloader
// through the new application.
bool fw_convert_hdplus(const fw_io_t *io, const fw_image_t *img);
// The running application's build (about block), for checking the result. False if it
// can't be read (bootloader, or firmware without the about block).
bool fw_read_build(const fw_io_t *io, char git[24], char built[20]);

#ifdef __cplusplus
}
#endif
