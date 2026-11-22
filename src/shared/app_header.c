// SPDX-License-Identifier: GPL-3.0-only
#include "app_header.h"

#ifndef XHD_HOST_TEST
#include "stm32.h"
#endif

#ifdef BUILD_APPLICATION
// All ones until the build stamps it (application.ld puts it right after the vector table).
__attribute__((used, section(".app_header")))
const app_header_t g_app_header = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
#endif

app_check_t app_image_check(const uint32_t *app, uint32_t region_bytes, app_crc_fn crc,
                            uint32_t *want, uint32_t *got)
{
    const app_header_t *h = (const app_header_t *)((const uint8_t *)app + APP_HEADER_OFFSET);
    *want = h->crc;
    *got = 0;
    if (h->magic != APP_HEADER_MAGIC) return APP_IMAGE_NO_HEADER;
    if (h->length < APP_HEADER_OFFSET + sizeof(app_header_t) || h->length > region_bytes ||
        (h->length & 3u)) {
        return APP_IMAGE_BAD;
    }
    const uint32_t crc_word = (APP_HEADER_OFFSET + 8u) / 4u;
    *got = crc(app, crc_word, app + crc_word + 1u, h->length / 4u - crc_word - 1u);
    return *got == h->crc ? APP_IMAGE_OK : APP_IMAGE_BAD;
}

#ifndef XHD_HOST_TEST
// Standard CRC-32 from the unit's fixed 0x04C11DB7 (MSB first): each word bit-reversed in
// (REV_IN = 11), the result bit-reversed out (REV_OUT), then inverted -- the same value as
// crc32_buf() over the words' little-endian bytes (run_app_header_fixtures models it).
uint32_t app_crc_hw(const uint32_t *a, uint32_t a_words, const uint32_t *b, uint32_t b_words)
{
    RCC->AHBENR |= RCC_AHBENR_CRCEN;
    CRC->INIT = 0xFFFFFFFFu;
    CRC->CR = CRC_CR_REV_IN | CRC_CR_REV_OUT | CRC_CR_RESET;
    while (a_words--) CRC->DR = *a++;
    while (b_words--) CRC->DR = *b++;
    const uint32_t c = CRC->DR ^ 0xFFFFFFFFu;
    RCC->AHBENR &= ~RCC_AHBENR_CRCEN;
    return c;
}
#endif
