// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// The application image header: right after the application's vector table, stamped by
// scripts/combine_firmware.py with the image's length and CRC-32. A build flashed without that
// step (the application ELF straight from an ST-Link) keeps the linker's all-ones header, which
// reads as "no header". The CRC covers [application start, start + length) except the crc
// field itself.
#define APP_HEADER_OFFSET 0xC0u          // the STM32F030x8 vector table: 48 words
#define APP_HEADER_MAGIC  0x31485841u    // "AXH1"
// What the CRC may cover: the application region up to the settings page (page 63).
#define APP_REGION_BYTES  (43u * 1024u)

typedef struct {
    uint32_t magic;
    uint32_t length;   // bytes, a multiple of 4
    uint32_t crc;      // standard CRC-32 (zlib)
} app_header_t;

typedef enum { APP_IMAGE_NO_HEADER, APP_IMAGE_OK, APP_IMAGE_BAD } app_check_t;

// CRC-32 of two runs of words, the second continuing the first.
typedef uint32_t (*app_crc_fn)(const uint32_t *a, uint32_t a_words, const uint32_t *b, uint32_t b_words);

// The image at `app` (word-aligned, `region_bytes` readable) against its header. *want / *got:
// the header's CRC and the computed one (0 when the length is out of bounds).
app_check_t app_image_check(const uint32_t *app, uint32_t region_bytes, app_crc_fn crc,
                            uint32_t *want, uint32_t *got);

#ifndef XHD_HOST_TEST
// app_crc_fn on the STM32F030's CRC unit: about 2 ms for the whole region.
uint32_t app_crc_hw(const uint32_t *a, uint32_t a_words, const uint32_t *b, uint32_t b_words);
#endif
